
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cstdio>
#include <cstring>

#include <iostream>
#include <vector>
#include <random>

using namespace std;

#include <gtest/gtest.h>

#include "kernel_init_funcs.h"

extern "C" {
   #include <tilck/kernel/fs/fat32.h>
   #include <tilck/kernel/fs/vfs.h>
   int check_mountpoint_match(const char *mp, u32 lm, const char *path, u32 lp);
}

static int mountpoint_match_wrapper(const char *mp, const char *path)
{
   return check_mountpoint_match(mp, strlen(mp), path, strlen(path));
}

// Implemented in fat32_test.cpp
const char *load_once_file(const char *filepath, size_t *fsize = nullptr);

TEST(vfs, check_mountpoint_match)
{
   EXPECT_EQ(mountpoint_match_wrapper("/", "/"), 1);
   EXPECT_EQ(mountpoint_match_wrapper("/", "/file"), 1);
   EXPECT_EQ(mountpoint_match_wrapper("/", "/dir1/file2"), 1);
   EXPECT_EQ(mountpoint_match_wrapper("/dev/", "/dev/tty0"), 5);
   EXPECT_EQ(mountpoint_match_wrapper("/devices/", "/dev"), 0);
   EXPECT_EQ(mountpoint_match_wrapper("/dev/", "/dev"), 4);
}

TEST(vfs, read_content_of_longname_file)
{
   init_kmalloc_for_tests();

   const char *buf = load_once_file(PROJ_BUILD_DIR "/fatpart");
   char data[128] = {0};

   filesystem *fat_fs = fat_mount_ramdisk((void *) buf, VFS_FS_RO);
   ASSERT_TRUE(fat_fs != NULL);

   int r = mountpoint_add(fat_fs, "/");
   ASSERT_EQ(r, 0);

   const char *file_path =
      "/testdir/This_is_a_file_with_a_veeeery_long_name.txt";

   fs_handle h = NULL;
   r = vfs_open(file_path, &h);
   ASSERT_TRUE(r == 0);
   ASSERT_TRUE(h != NULL);
   ssize_t res = vfs_read(h, data, sizeof(data));
   vfs_close(h);

   EXPECT_GT(res, 0);
   ASSERT_STREQ("Content of file with a long name\n", data);

   mountpoint_remove(fat_fs);
   fat_umount_ramdisk(fat_fs);
}

TEST(vfs, fseek)
{
   init_kmalloc_for_tests();

   random_device rdev;
   const auto seed = rdev();
   default_random_engine engine(seed);
   lognormal_distribution<> dist(4.0, 3);
   off_t linux_lseek;
   off_t tilck_fseek;
   off_t linux_pos;
   off_t tilck_pos;

   cout << "[ INFO     ] random seed: " << seed << endl;

   size_t fatpart_size;
   const char *fatpart =
      load_once_file(PROJ_BUILD_DIR "/fatpart", &fatpart_size);

   filesystem *fat_fs = fat_mount_ramdisk((void *) fatpart, VFS_FS_RO);
   ASSERT_TRUE(fat_fs != NULL);

   int r = mountpoint_add(fat_fs, "/");
   ASSERT_EQ(r, 0);

   const char *fatpart_file_path = "/EFI/BOOT/elf_kernel_stripped";
   const char *real_file_path =
      PROJ_BUILD_DIR "/sysroot/EFI/BOOT/elf_kernel_stripped";

   int fd = open(real_file_path, O_RDONLY);

   size_t file_size = lseek(fd, 0, SEEK_END);

   fs_handle h = NULL;
   r = vfs_open(fatpart_file_path, &h);
   ASSERT_TRUE(r == 0);
   ASSERT_TRUE(h != NULL);

   char buf_tilck[64];
   char buf_linux[64];

   linux_lseek = lseek(fd, file_size / 2, SEEK_SET);
   tilck_fseek = vfs_seek(h, file_size / 2, SEEK_SET);
   ASSERT_EQ(linux_lseek, tilck_fseek);

   off_t last_pos = lseek(fd, 0, SEEK_CUR);

   linux_pos = last_pos;
   tilck_pos = vfs_seek(h, 0, SEEK_CUR);
   ASSERT_EQ(linux_pos, tilck_pos);

   (void)last_pos;

   const int iters = 10000;

   for (int i = 0; i < iters; i++) {

      int saved_errno = 0;
      (void)saved_errno;

      off_t offset = (off_t) ( dist(engine) - dist(engine)/1.3 );

      linux_lseek = lseek(fd, offset, SEEK_CUR);
      tilck_fseek = vfs_seek(h, offset, SEEK_CUR);

      if (linux_lseek < 0)
         saved_errno = errno;

      off_t linux_pos = lseek(fd, 0, SEEK_CUR);
      off_t tilck_pos = vfs_seek(h, 0, SEEK_CUR);

      if (linux_lseek < 0) {

         /*
          * Linux syscalls return -ERRNO_VALUE in case something goes wrong,
          * while the glibc wrappers return -1 and set errno. Since we're
          * testing the value returned by the syscall in Tilck, we need to revert
          * that.
          */
         linux_lseek = -errno;
      }

      ASSERT_EQ(tilck_fseek, linux_lseek)
         << "Offset: " << offset << endl
         << "Curr pos (glibc): " << linux_pos << endl
         << "Curr pos (tilck):  " << tilck_pos << endl;

      ASSERT_EQ(tilck_pos, linux_pos);

      memset(buf_linux, 0, sizeof(buf_linux));
      memset(buf_tilck, 0, sizeof(buf_tilck));

      ssize_t linux_read = read(fd, buf_linux, sizeof(buf_linux));
      ssize_t tilck_read = vfs_read(h, buf_tilck, sizeof(buf_tilck));

      ASSERT_EQ(tilck_read, linux_read);

      linux_pos = lseek(fd, 0, SEEK_CUR);
      tilck_pos = vfs_seek(h, 0, SEEK_CUR);

      ASSERT_EQ(tilck_pos, linux_pos);

      if (memcmp(buf_tilck, buf_linux, sizeof(buf_linux)) != 0) {

         cout << "Buffers differ. " << endl;
         cout << "Last offset: " << offset << endl;
         cout << "Curr pos: " << linux_pos << endl;
         cout << "read ret: " << linux_read << endl;

         cout << "Linux buf: ";

         for (size_t i = 0; i < sizeof(buf_linux); i++)
            printf("%02x ", (u8)buf_linux[i]);

         cout << endl;
         cout << "Tilck buf:  ";

         for (size_t i = 0; i < sizeof(buf_linux); i++)
            printf("%02x ", (u8)buf_tilck[i]);

         cout << endl;
         FAIL();
      }

      last_pos = linux_pos;
   }

   vfs_close(h);
   close(fd);

   mountpoint_remove(fat_fs);
   fat_umount_ramdisk(fat_fs);
}


string compute_abs_path_wrapper(const char *cwd, const char *path)
{
   char dest[256];
   int rc = compute_abs_path(path, cwd, dest, sizeof(dest));

   if (rc < 0)
      return "<error>";

   return dest;
}

TEST(compute_abs_path, tests)
{
   /* path is absolute */
   EXPECT_EQ(compute_abs_path_wrapper("/", "/a/b/c"), "/a/b/c");
   EXPECT_EQ(compute_abs_path_wrapper("/", "/a/b/c/"), "/a/b/c");
   EXPECT_EQ(compute_abs_path_wrapper("/", "/a/b/c/.."), "/a/b");
   EXPECT_EQ(compute_abs_path_wrapper("/", "/a/b/c/../"), "/a/b");

   /* path is relative */
   EXPECT_EQ(compute_abs_path_wrapper("/", "a/b/c"), "/a/b/c");
   EXPECT_EQ(compute_abs_path_wrapper("/", "a/b/c/"), "/a/b/c");
   EXPECT_EQ(compute_abs_path_wrapper("/", "a/b/c/.."), "/a/b");
   EXPECT_EQ(compute_abs_path_wrapper("/", "a/b/c/../"), "/a/b");

   /* path is relative starting with ./ */
   EXPECT_EQ(compute_abs_path_wrapper("/", "./a/b/c"), "/a/b/c");
   EXPECT_EQ(compute_abs_path_wrapper("/", "./a/b/c/"), "/a/b/c");
   EXPECT_EQ(compute_abs_path_wrapper("/", "./a/b/c/.."), "/a/b");
   EXPECT_EQ(compute_abs_path_wrapper("/", "./a/b/c/../"), "/a/b");

   /* path is relative, cwd != / */
   EXPECT_EQ(compute_abs_path_wrapper("/a/b/c/", "a"), "/a/b/c/a");
   EXPECT_EQ(compute_abs_path_wrapper("/a/b/c/", "a/"), "/a/b/c/a");
   EXPECT_EQ(compute_abs_path_wrapper("/a/b/c/", ".."), "/a/b");
   EXPECT_EQ(compute_abs_path_wrapper("/a/b/c/", "../"), "/a/b");
   EXPECT_EQ(compute_abs_path_wrapper("/a/b/c/", "../.."), "/a");
   EXPECT_EQ(compute_abs_path_wrapper("/a/b/c/", "../../"), "/a");
   EXPECT_EQ(compute_abs_path_wrapper("/a/b/c/", "../../."), "/a");
   EXPECT_EQ(compute_abs_path_wrapper("/a/b/c/", "../.././"), "/a");
   EXPECT_EQ(compute_abs_path_wrapper("/a/b/c/", "../../.."), "/");
   EXPECT_EQ(compute_abs_path_wrapper("/a/b/c/", "../../../"), "/");

   /* try to go beyond / */
   EXPECT_EQ(compute_abs_path_wrapper("/a/b/c/", "../../../.."), "/");
   EXPECT_EQ(compute_abs_path_wrapper("/a/b/c/", "../../../../"), "/");

   /* double slash */
   EXPECT_EQ(compute_abs_path_wrapper("/a/b/c/", "d//e"), "/a/b/c/d/e");

   /* triple slash */
   EXPECT_EQ(compute_abs_path_wrapper("/a/b/c/", "d///e"), "/a/b/c/d/e");

   /* other */
   EXPECT_EQ(compute_abs_path_wrapper("/a/b/c/", ".a"), "/a/b/c/.a");
   EXPECT_EQ(compute_abs_path_wrapper("/a/b/c/", "..a"), "/a/b/c/..a");
}
