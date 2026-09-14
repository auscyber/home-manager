// Simple wrapper to activate launchd sockets and expose them to the
// child process using the same LISTEN_PID / LISTEN_FDS / LISTEN_FDNAMES
// protocol that systemd uses, so that programs like gpg-agent can be run
// in --supervised mode under launchd.

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <launch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Per the sd_listen_fds(3) protocol, the first passed FD is 3.
#define LISTEN_FDS_START 3

// Activate the named launchd socket and place its (single) FD at
// target_fd, clearing FD_CLOEXEC so it survives exec. Returns 1 on
// success, 0 on failure.
static int activate_socket(const char *name, int target_fd) {
  int *fds = NULL;
  size_t count = 0;
  int rc = launch_activate_socket(name, &fds, &count);
  if (rc != 0) {
    errno = rc;
    warn("launch_activate_socket(%s)", name);
    return 0;
  }
  if (fds == NULL || count < 1) {
    warnx("launch_activate_socket(%s): no file descriptors returned", name);
    free(fds);
    return 0;
  }
  if (count != 1) {
    warnx("Expected one FD from launchd for %s, got %zu; using the first.",
          name, count);
  }

  if (fds[0] != target_fd) {
    if (dup2(fds[0], target_fd) < 0) {
      warn("dup2");
      for (size_t i = 0; i < count; i++)
        close(fds[i]);
      free(fds);
      return 0;
    }
    close(fds[0]);
  }

  // dup2 clears FD_CLOEXEC on the new fd, but if fds[0] == target_fd we
  // skipped the dup2 and still need to clear it explicitly.
  int flags = fcntl(target_fd, F_GETFD);
  if (flags >= 0 && (flags & FD_CLOEXEC)) {
    fcntl(target_fd, F_SETFD, flags & ~FD_CLOEXEC);
  }

  // Close any extra FDs we don't intend to forward.
  for (size_t i = 1; i < count; i++) {
    close(fds[i]);
  }
  free(fds);
  return 1;
}

static void print_usage(const char *prog) {
  fprintf(stderr,
          "Usage: %s [-s <socket>]... [--] <command> [args...]\n"
          "\n"
          "  -s <socket name>  Activate the named launchd socket. May be\n"
          "                    given multiple times; sockets are passed to\n"
          "                    the child in the order given.\n"
          "\n"
          "Activates launchd sockets and sets LISTEN_PID, LISTEN_FDS, and\n"
          "LISTEN_FDNAMES so the child can consume them like a systemd\n"
          "socket-activated service.\n",
          prog);
}

int main(int argc, char **argv) {
  const char *prog = argv[0];

  const char **sockets = NULL;
  int sockets_len = 0;
  int sockets_cap = 0;

  int i = 1;
  while (i < argc && argv[i][0] == '-' && argv[i][1] != '\0') {
    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      print_usage(prog);
      free(sockets);
      return 0;
    }
    if (strcmp(argv[i], "-s") == 0) {
      if (i + 1 >= argc) {
        warnx("Expected socket name after -s");
        print_usage(prog);
        free(sockets);
        return 1;
      }
      if (sockets_len == sockets_cap) {
        sockets_cap = sockets_cap ? sockets_cap * 2 : 8;
        const char **tmp = realloc(sockets, sizeof(*sockets) * sockets_cap);
        if (!tmp)
          err(1, "realloc");
        sockets = tmp;
      }
      sockets[sockets_len++] = argv[i + 1];
      i += 2;
      continue;
    }
    warnx("Unknown option: %s", argv[i]);
    print_usage(prog);
    free(sockets);
    return 1;
  }

  if (i >= argc) {
    warnx("No command specified");
    print_usage(prog);
    free(sockets);
    return 1;
  }

  // Activate sockets and dup them to consecutive FDs starting at 3.
  int next_fd = LISTEN_FDS_START;
  char *names = NULL;
  size_t names_len = 0;

  for (int j = 0; j < sockets_len; j++) {
    if (!activate_socket(sockets[j], next_fd)) {
      free(sockets);
      free(names);
      return 1;
    }
    size_t add = strlen(sockets[j]);
    char *tmp = realloc(names, names_len + add + 2);
    if (!tmp)
      err(1, "ran out of memory");
    names = tmp;
    if (names_len > 0)
      names[names_len++] = ':';
    memcpy(names + names_len, sockets[j], add);
    names_len += add;
    names[names_len] = '\0';
    next_fd++;
  }
  free(sockets);

  char buf[32];
  snprintf(buf, sizeof(buf), "%ld", (long)getpid());
  setenv("LISTEN_PID", buf, 1);

  snprintf(buf, sizeof(buf), "%d", next_fd - LISTEN_FDS_START);
  setenv("LISTEN_FDS", buf, 1);

  setenv("LISTEN_FDNAMES", names ? names : "", 1);
  free(names);

  execvp(argv[i], &argv[i]);
  err(1, "execvp: %s", argv[i]);
}
