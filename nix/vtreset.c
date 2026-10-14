#include <fcntl.h>
#include <linux/kd.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int fd, opened = 0, rc = 0;

    if (argc > 1) {
        fd = open(argv[1], O_RDWR | O_NOCTTY | O_CLOEXEC);
        opened = 1;
    } else if (isatty(STDIN_FILENO)) {
        fd = STDIN_FILENO;
    } else {
        fd = open("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC);
        opened = 1;
    }
    if (fd < 0) {
        perror("fbbrowser-vt-reset: open");
        return 1;
    }

    if (ioctl(fd, KDSETMODE, KD_TEXT) < 0) {
        perror("fbbrowser-vt-reset: KDSETMODE");
        rc = 1;
    }
    if (ioctl(fd, KDSKBMODE, K_UNICODE) < 0) {
        perror("fbbrowser-vt-reset: KDSKBMODE");
        rc = 1;
    }

    if (opened)
        close(fd);
    return rc;
}
