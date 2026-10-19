/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

// File system test for a newlib program, through lib/lku and the namespace and file services:
// reads a file shipped in the image with stdio, writes, appends to, seeks in and truncates a
// file of its own, makes a directory and lists it, and checks errno where calls fail. Removes
// what it made, and first what a run that died part way left. Prints POSIXTEST: PASS and
// returns 0 when every check holds; each failed check prints a POSIXTEST: FAIL line and main
// returns 1.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define DATA_FILE "/data/posixtest.txt"
#define OUT_FILE  "/posixtest.out"
#define OUT_DIR   "/posixtest.dir"

// what posixtest.txt holds
static const char data_text[] = "line one\nline two\nthe third line is longer than the others\n";
#define DATA_LEN ((long)sizeof(data_text) - 1)

static const char *const dir_names[] = {"one", "two", "three"};
#define NDIR_NAMES 3

static int failures;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            printf("POSIXTEST: FAIL %s:%d: %s (errno %d)\n", __FILE__, __LINE__, #cond, \
                   errno);                                                             \
            failures++;                                                                \
        }                                                                              \
    } while (0)

static void dir_path(char *buf, size_t len, const char *name) {
    snprintf(buf, len, "%s/%s", OUT_DIR, name);
}

static void cleanup(void) {
    char path[64];
    unlink(OUT_FILE);
    for (int i = 0; i < NDIR_NAMES; i++) {
        dir_path(path, sizeof(path), dir_names[i]);
        unlink(path);
    }
    rmdir(OUT_DIR);
}

static void test_read(void) {
    FILE *f = fopen(DATA_FILE, "r");
    CHECK(f != NULL);
    if (!f) {
        return;
    }
    char line[64];
    CHECK(fgets(line, sizeof(line), f) && strcmp(line, "line one\n") == 0);
    CHECK(fgets(line, sizeof(line), f) && strcmp(line, "line two\n") == 0);

    // all of it, from the start
    char buf[128];
    CHECK(fseek(f, 0, SEEK_SET) == 0);
    size_t n = fread(buf, 1, sizeof(buf), f);
    CHECK(n == (size_t)DATA_LEN && memcmp(buf, data_text, n) == 0);
    CHECK(feof(f));
    CHECK(fseek(f, 0, SEEK_END) == 0 && ftell(f) == DATA_LEN);
    CHECK(fseek(f, 5, SEEK_SET) == 0 && fgetc(f) == 'o');

    struct stat st;
    CHECK(fstat(fileno(f), &st) == 0 && S_ISREG(st.st_mode) && st.st_size == DATA_LEN);
    CHECK(fclose(f) == 0);

    CHECK(stat(DATA_FILE, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == DATA_LEN);
    CHECK(stat("/data", &st) == 0 && S_ISDIR(st.st_mode));
    // relative to "/", the only working directory
    CHECK(stat("data/posixtest.txt", &st) == 0 && st.st_size == DATA_LEN);

    errno = 0;
    CHECK(fopen("/data/missing.txt", "r") == NULL && errno == ENOENT);
    errno = 0;
    CHECK(open("/data", O_WRONLY) < 0 && errno == EISDIR);
    errno = 0;
    CHECK(stat("/data/posixtest.txt/x", &st) < 0);
    printf("POSIXTEST: read ok\n");
}

static void test_write(void) {
    // written with stdio, read back with read()
    FILE *f = fopen(OUT_FILE, "w");
    CHECK(f != NULL);
    if (!f) {
        return;
    }
    fprintf(f, "hello %d\n", 42);
    fputs("second line\n", f);
    CHECK(fclose(f) == 0);

    char buf[64];
    int fd = open(OUT_FILE, O_RDONLY);
    CHECK(fd >= 0);
    CHECK(read(fd, buf, sizeof(buf)) == 21 && memcmp(buf, "hello 42\nsecond line\n", 21) == 0);
    CHECK(read(fd, buf, sizeof(buf)) == 0);
    errno = 0;
    CHECK(write(fd, "x", 1) < 0 && errno == EBADF);
    CHECK(close(fd) == 0);

    // appended to
    f = fopen(OUT_FILE, "a");
    CHECK(f != NULL);
    if (f) {
        fputs("third\n", f);
        CHECK(fclose(f) == 0);
    }
    struct stat st;
    CHECK(stat(OUT_FILE, &st) == 0 && st.st_size == 27);

    // written in the middle, sought around, cut short
    fd = open(OUT_FILE, O_RDWR);
    CHECK(fd >= 0);
    CHECK(lseek(fd, 6, SEEK_SET) == 6);
    CHECK(write(fd, "XY", 2) == 2);
    CHECK(lseek(fd, 0, SEEK_CUR) == 8);
    CHECK(lseek(fd, -6, SEEK_END) == 21);
    CHECK(read(fd, buf, sizeof(buf)) == 6 && memcmp(buf, "third\n", 6) == 0);
    CHECK(ftruncate(fd, 8) == 0);
    CHECK(fstat(fd, &st) == 0 && st.st_size == 8);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    CHECK(read(fd, buf, sizeof(buf)) == 8 && memcmp(buf, "hello XY", 8) == 0);
    errno = 0;
    CHECK(lseek(fd, -100, SEEK_CUR) < 0 && errno == EINVAL);
    CHECK(close(fd) == 0);

    errno = 0;
    CHECK(open(OUT_FILE, O_WRONLY | O_CREAT | O_EXCL, 0644) < 0 && errno == EEXIST);
    fd = open(OUT_FILE, O_WRONLY | O_TRUNC);
    CHECK(fd >= 0 && fstat(fd, &st) == 0 && st.st_size == 0);
    CHECK(close(fd) == 0);
    CHECK(truncate(OUT_FILE, 0) == 0);

    CHECK(unlink(OUT_FILE) == 0);
    errno = 0;
    CHECK(stat(OUT_FILE, &st) < 0 && errno == ENOENT);
    errno = 0;
    CHECK(unlink(OUT_FILE) < 0 && errno == ENOENT);
    printf("POSIXTEST: write ok\n");
}

// readdir's entries, other than . and .., counted against dir_names
static int count_entries(DIR *d, int seen[NDIR_NAMES]) {
    int others = 0;
    struct dirent *e;
    for (int i = 0; i < NDIR_NAMES; i++) {
        seen[i] = 0;
    }
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        CHECK(e->d_ino != 0);
        int i = 0;
        while (i < NDIR_NAMES && strcasecmp(e->d_name, dir_names[i]) != 0) {
            i++;
        }
        if (i < NDIR_NAMES) {
            seen[i]++;
        } else {
            others++;
        }
    }
    return others;
}

static void test_dirs(void) {
    CHECK(mkdir(OUT_DIR, 0755) == 0);
    errno = 0;
    CHECK(mkdir(OUT_DIR, 0755) < 0 && errno == EEXIST);

    char path[64];
    for (int i = 0; i < NDIR_NAMES; i++) {
        dir_path(path, sizeof(path), dir_names[i]);
        FILE *f = fopen(path, "w");
        CHECK(f != NULL);
        if (f) {
            fputs(dir_names[i], f);
            CHECK(fclose(f) == 0);
        }
    }

    DIR *d = opendir(OUT_DIR);
    CHECK(d != NULL);
    if (d) {
        int seen[NDIR_NAMES];
        CHECK(count_entries(d, seen) == 0);
        for (int i = 0; i < NDIR_NAMES; i++) {
            CHECK(seen[i] == 1);
        }
        // and again from the start
        rewinddir(d);
        CHECK(count_entries(d, seen) == 0);
        for (int i = 0; i < NDIR_NAMES; i++) {
            CHECK(seen[i] == 1);
        }
        CHECK(closedir(d) == 0);
    }

    struct stat st;
    CHECK(stat(OUT_DIR, &st) == 0 && S_ISDIR(st.st_mode));
    int fd = open(OUT_DIR, O_RDONLY);
    CHECK(fd >= 0 && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode));
    CHECK(close(fd) == 0);
    errno = 0;
    CHECK(opendir(DATA_FILE) == NULL && errno == ENOTDIR);
    errno = 0;
    CHECK(rmdir(OUT_DIR) < 0 && errno == ENOTEMPTY);
    errno = 0;
    CHECK(unlink(OUT_DIR) < 0 && errno == EISDIR);

    for (int i = 0; i < NDIR_NAMES; i++) {
        dir_path(path, sizeof(path), dir_names[i]);
        CHECK(unlink(path) == 0);
    }
    CHECK(rmdir(OUT_DIR) == 0);
    errno = 0;
    CHECK(stat(OUT_DIR, &st) < 0 && errno == ENOENT);
    printf("POSIXTEST: directories ok\n");
}

static void test_misc(void) {
    CHECK(isatty(1) == 1);
    int fd = open(DATA_FILE, O_RDONLY);
    CHECK(fd >= 0 && isatty(fd) == 0);

    // the lowest free fd goes first
    int fd2 = open(DATA_FILE, O_RDONLY);
    CHECK(fd2 == fd + 1);
    CHECK(close(fd) == 0);
    CHECK(open(DATA_FILE, O_RDONLY) == fd);
    CHECK(close(fd) == 0);
    CHECK(close(fd2) == 0);
    errno = 0;
    CHECK(close(fd2) < 0 && errno == EBADF);
    errno = 0;
    CHECK(close(99) < 0 && errno == EBADF);

    CHECK(getpid() > 0);
    struct timeval t0, t1;
    CHECK(gettimeofday(&t0, NULL) == 0);
    CHECK(usleep(2000) == 0);
    CHECK(gettimeofday(&t1, NULL) == 0);
    const long long us = (t1.tv_sec - t0.tv_sec) * 1000000ll + (t1.tv_usec - t0.tv_usec);
    CHECK(us >= 2000);
    printf("POSIXTEST: misc ok\n");
}

int main(void) {
    printf("POSIXTEST: start\n");
    cleanup();

    test_read();
    test_write();
    test_dirs();
    test_misc();

    if (failures) {
        printf("POSIXTEST: FAIL (%d checks)\n", failures);
        return 1;
    }
    printf("POSIXTEST: PASS\n");
    return 0;
}
