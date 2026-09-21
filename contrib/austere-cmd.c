/* usage: austere-cmd action [arg...]
 * Sends one command line to the austere command socket (§8) and prints
 * the reply. Exit 0 on "ok", 1 on "err"/failure. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s action [arg...]\n", argv[0]);
        return 1;
    }
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    char path[512];

    if (xdg && *xdg) {
        if (snprintf(path, sizeof(path), "%s/austere/socket", xdg) >=
            (int)sizeof(path))
            return 1;
    } else {
        snprintf(path, sizeof(path), "/tmp/austere-%u/socket",
            (unsigned)getuid());
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0)
        return 1;
    struct sockaddr_un sa = { 0 };

    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("austere-cmd: connect");
        return 1;
    }
    char line[1024];
    int o = 0;

    for (int i = 1; i < argc; i++) {
        int need = snprintf(line + o, sizeof(line) - (size_t)o, "%s%s",
            i > 1 ? " " : "", argv[i]);

        if (need < 0)
            break;
        o += need;
        if (o >= (int)sizeof(line) - 2) {
            o = (int)sizeof(line) - 2;
            break;
        }
    }
    line[o] = '\n';
    write(fd, line, (size_t)o + 1);
    shutdown(fd, SHUT_WR);

    char reply[1024];
    ssize_t n = read(fd, reply, sizeof(reply) - 1);

    if (n <= 0)
        return 1;
    reply[n] = '\0';
    fputs(reply, stdout);
    int rc = strncmp(reply, "ok", 2) == 0 ? 0 : 1;
    close(fd);
    return rc;
}
