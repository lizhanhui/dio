#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#include <cstdint>
#endif

#include <ctime>
#include <fcntl.h>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include "gflags/gflags.h"

inline long current_time_nano() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000000000 + ts.tv_nsec;
}

void observe(uint32_t *histogram, long elapsed) {
  int index = elapsed / 1000000;
  if (index >= 100) {
    index = 100;
  }
  histogram[index]++;
}

void report(uint32_t *histogram) {
  for (int i = 0; i < 101; i++) {
    if (histogram[i]) {
      printf("[%d, %d): %d\n", i, i + 1, histogram[i]);
    }
  }

  for (int i = 1; i < 101; i++) {
    histogram[i] += histogram[i - 1];
  }
  float percentiles[] = {0.5, 0.9, 0.99, 0.999};
  const char *labels[] = {"p50", "p90", "p99", "p999"};
  for (int i = 0; i < 4; i++) {
    uint32_t index = (uint32_t)(percentiles[i] * histogram[100]);
    for (int j = 100; j >= 0; j--) {
      if (histogram[j] <= index) {
        printf("%s: %d+ms\n", labels[i], j);
        break;
      }
    }
  }
}

DEFINE_string(file_name, "/data/test", "Test File Name");
DEFINE_int64(file_len, 1024, "File Length in MiB");
DEFINE_int32(block_size, 4096, "Block Size");
DEFINE_int32(queue_depth, 32, "Queue Depth");

int main(int argc, char *argv[]) {
  gflags::SetUsageMessage("dio -file_name /data/test -file_len 1024 "
                          "-block_size 4096 -queue_depth 32");
  gflags::SetVersionString("1.0.0");
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  long file_len = (1 << 20) * FLAGS_file_len;
  int block_size = FLAGS_block_size;
  int queue_depth = FLAGS_queue_depth;

  printf("file-name: %s, file-len: %ldMiB, block-size: %d, queue-depth: %d\n",
         FLAGS_file_name.c_str(), FLAGS_file_len, FLAGS_block_size,
         FLAGS_queue_depth);

  int fd = open(FLAGS_file_name.c_str(), O_RDWR | O_CREAT | O_DSYNC | O_DIRECT,
                0644);
  if (fd < 0) {
    fprintf(stderr, "open: %s\n", strerror(errno));
    return EXIT_FAILURE;
  }

  int ret = fallocate(fd, 0, 0, file_len);
  if (ret < 0) {
    fprintf(stderr, "fallocate: %s\n", strerror(errno));
    return EXIT_FAILURE;
  }

  int alignment = 4096;
  void *ptr = NULL;
  ret = posix_memalign(&ptr, alignment, block_size);
  if (ret) {
    fprintf(stderr, "posix_memalign: %s\n", strerror(ret));
    return EXIT_FAILURE;
  }
  memset(ptr, 65, block_size);

  struct iovec iov = {};
  iov.iov_base = ptr;
  iov.iov_len = block_size;
  ret = pwritev(fd, &iov, 1, 0);
  if (ret < 0) {
    fprintf(stderr, "pwritev: %s\n", strerror(errno));
    return EXIT_FAILURE;
  }

  struct io_uring ring;
  struct io_uring_params params;
  memset(&params, 0, sizeof(params));
  params.flags |= IORING_SETUP_SQPOLL | IORING_SETUP_IOPOLL |
                  IORING_SETUP_SQ_AFF | IORING_SETUP_CQSIZE;
  params.sq_thread_idle = 2000;
  params.sq_thread_cpu = 1;
  params.cq_entries = queue_depth;
  ret = io_uring_queue_init_params(queue_depth, &ring, &params);
  if (ret < 0) {
    fprintf(stderr, "io_uring_queue_init_params: %s\n", strerror(-ret));
    return EXIT_FAILURE;
  }

  long offset = 0;
  int writes = 0;
  struct io_uring_cqe *cqe;

  uint32_t histogram[101] = {0};

  // main loop
  while (true) {

    // All writes are done
    if (offset >= file_len && writes == 0) {
      fprintf(stdout, "All writes are done\n");
      break;
    }

    int prepared = 0;

    // Try to build and submit SQEs
    for (;;) {
      if (offset >= file_len) {
        break;
      }

      if (writes + prepared >= queue_depth) {
        break;
      }

      struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
      if (!sqe) {
        break;
      }

      io_uring_sqe_set_data(sqe, (void *)current_time_nano());

      io_uring_prep_writev(sqe, fd, &iov, 1, offset);
      offset += block_size;
      prepared++;
    }

    if (prepared) {
      ret = io_uring_submit(&ring);
      if (ret < 0) {
        fprintf(stderr, "io_uring_submit: %s\n", strerror(-ret));
        goto quit;
      }
      writes += prepared;
    }

    // Try to complete CQEs
    bool reaped = false;
    for (;;) {

      if (!reaped) {
        ret = io_uring_wait_cqe(&ring, &cqe);
        reaped = true;
      } else {
        ret = io_uring_peek_cqe(&ring, &cqe);
        if (ret == -EAGAIN) {
          ret = 0;
          cqe = NULL;
        }
      }

      if (ret < 0) {
        fprintf(stderr, "io_uring_peek_cqe: %s\n", strerror(-ret));
        goto quit;
      }

      if (!cqe) {
        break;
      }

      if (cqe->res < 0) {
        fprintf(stderr, "io_uring_peek_cqe: %s\n", strerror(-cqe->res));
        goto quit;
      }

      long elapsed = current_time_nano() - (long)io_uring_cqe_get_data(cqe);
      observe(histogram, elapsed);
      io_uring_cqe_seen(&ring, cqe);
      writes--;
    }
  }

  report(histogram);

quit:
  io_uring_queue_exit(&ring);
  close(fd);

  return EXIT_SUCCESS;
}