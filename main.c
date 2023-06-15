#define _GNU_SOURCE

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

int main(int argc, char *argv[]) {
  if (argc < 2) {
    printf("Usage: %s <filename>\n", argv[0]);
    return EXIT_FAILURE;
  }

  int fd = open(argv[1], O_RDWR | O_CREAT | O_DSYNC | O_DIRECT, 0644);
  if (fd < 0) {
    fprintf(stderr, "open: %s\n", strerror(errno));
    return EXIT_FAILURE;
  }

  long file_len = 1 << 30;
  int ret = fallocate(fd, 0, 0, file_len);
  if (ret < 0) {
    fprintf(stderr, "fallocate: %s\n", strerror(errno));
    return EXIT_FAILURE;
  }

  int block_size = 4096;
  int alignment = 4096;
  void *ptr = NULL;
  ret = posix_memalign(&ptr, 4096, block_size);
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

  int queue_depth = 1024;

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

      io_uring_cqe_seen(&ring, cqe);
      writes--;
    }
  }

quit:
  io_uring_queue_exit(&ring);
  close(fd);

  return EXIT_SUCCESS;
}