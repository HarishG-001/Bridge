#define _XOPEN_SOURCE 600
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <signal.h>
#include <time.h>

#define PORT 5000
#define BUF_SIZE 256
#define LINE_SIZE 64

static int sockfd = -1, client_fd = -1, master_fd = -1, slave_fd = -1;
static pid_t getty_pid = -1;
static time_t getty_started;

static volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t child_exited;

/* Echo suppression state: drop the PTY output that follows a TCP->PTY write */
static int skip_echo;

/* Tracks the line typed by the client so "exit"/"quit" match whole lines only */
static char line[LINE_SIZE];
static size_t line_len;

static void on_signal(int sig)
{
    if (sig == SIGCHLD)
        child_exited = 1;
    else
        stop_requested = 1;
}

static void die(const char *msg)
{
    perror(msg);
    exit(1);
}

static int write_all(int fd, const char *p, size_t n)
{
    while (n > 0)
    {
        ssize_t w = write(fd, p, n);
        if (w < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static void start_getty(void)
{
    getty_started = time(NULL);
    getty_pid = fork();
    if (getty_pid < 0)
    {
        perror("fork");
        return;
    }

    if (getty_pid == 0)
    {
        setsid();  /* own process group so the whole login session can be killed */
        signal(SIGPIPE, SIG_DFL);

        close(master_fd);
        close(sockfd);
        if (client_fd >= 0)
            close(client_fd);

        dup2(slave_fd, STDIN_FILENO);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);
        if (slave_fd > STDERR_FILENO)
            close(slave_fd);

        execl("/sbin/getty", "getty", "-L", "ttyS5", "115200", "vt100", (char *)NULL);

        perror("exec getty failed");
        _exit(1);
    }
}

static void stop_getty(void)
{
    if (getty_pid > 0)
    {
        kill(-getty_pid, SIGKILL);
        waitpid(getty_pid, NULL, 0);
        getty_pid = -1;
    }
}

static void restart_getty(void)
{
    stop_getty();
    start_getty();
}

/* Respawn getty after it (or the login session it became) has ended */
static void reap_getty(void)
{
    if (getty_pid > 0 && waitpid(getty_pid, NULL, WNOHANG) == getty_pid)
    {
        getty_pid = -1;
        if (time(NULL) - getty_started < 1)
            sleep(1);  /* avoid a tight respawn loop if getty keeps failing */
        start_getty();
    }
}

static void drop_client(void)
{
    if (client_fd >= 0)
        close(client_fd);
    client_fd = -1;
    line_len = 0;
    skip_echo = 0;
}

static void cleanup(void)
{
    stop_getty();
    drop_client();
    if (sockfd >= 0)
        close(sockfd);
    if (master_fd >= 0)
        close(master_fd);
    if (slave_fd >= 0)
        close(slave_fd);
    unlink("/dev/ttyS5");
    printf("\nCleaned up resources.\n");
}

static void configure_serial(int fd)
{
    struct termios tty;

    if (tcgetattr(fd, &tty) != 0)
    {
        perror("tcgetattr");
        return;
    }

    tcflush(fd, TCIOFLUSH);

    cfsetospeed(&tty, B115200);
    cfsetispeed(&tty, B115200);

    tty.c_cflag &= ~PARENB;  /* no parity */
    tty.c_cflag &= ~CSTOPB;  /* 1 stop bit */
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;      /* 8 data bits */

    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHONL | ISIG | IEXTEN);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_oflag &= ~OPOST;

    tcflush(fd, TCIFLUSH);

    if (tcsetattr(fd, TCSANOW, &tty) != 0)
        perror("tcsetattr");
}

/* Feeds client bytes to the line tracker; sets *do_exit / *do_quit on a full-line command */
static void scan_commands(const char *buf, int n, int *do_exit, int *do_quit)
{
    for (int i = 0; i < n; i++)
    {
        unsigned char c = (unsigned char)buf[i];

        if (c == '\r' || c == '\n')
        {
            if (line_len == 4 && memcmp(line, "exit", 4) == 0)
                *do_exit = 1;
            else if (line_len == 4 && memcmp(line, "quit", 4) == 0)
                *do_quit = 1;
            line_len = 0;
        }
        else if (c == 0x7f || c == 0x08)
        {
            if (line_len > 0)
                line_len--;
        }
        else
        {
            if (line_len < LINE_SIZE)
                line[line_len] = (char)c;
            line_len++;
        }
    }
}

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NOCLDSTOP;  /* no SA_RESTART so select() wakes on signals */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    char slave_name[100];

    master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (master_fd < 0)
        die("posix_openpt");
    if (grantpt(master_fd) < 0)
        die("grantpt");
    if (unlockpt(master_fd) < 0)
        die("unlockpt");
    if (ptsname_r(master_fd, slave_name, sizeof(slave_name)) != 0)
        die("ptsname_r");

    /* Kept open for the whole run so the master never sees EIO when getty exits */
    slave_fd = open(slave_name, O_RDWR | O_NOCTTY);
    if (slave_fd < 0)
        die("open(slave)");

    printf("PTY created: %s\n", slave_name);

    unlink("/dev/ttyS5");
    if (symlink(slave_name, "/dev/ttyS5") != 0)
        perror("symlink(/dev/ttyS5)");  /* continue even if symlink fails */

    configure_serial(slave_fd);

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        die("socket");

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        die("bind");
    if (listen(sockfd, 1) < 0)
        die("listen");

    start_getty();
    printf("Listening on TCP port %d...\n", PORT);

    char buf[BUF_SIZE];

    while (!stop_requested)
    {
        if (child_exited)
        {
            child_exited = 0;
            reap_getty();
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sockfd, &fds);
        int maxfd = sockfd;
        if (client_fd >= 0)
        {
            FD_SET(client_fd, &fds);
            FD_SET(master_fd, &fds);
            if (client_fd > maxfd)
                maxfd = client_fd;
            if (master_fd > maxfd)
                maxfd = master_fd;
        }

        /* Timeout covers a SIGCHLD arriving between the flag check and select() */
        struct timeval tv = { 1, 0 };
        int ret = select(maxfd + 1, &fds, NULL, NULL, &tv);
        if (ret < 0)
        {
            if (errno == EINTR)
                continue;
            perror("select");
            break;
        }

        /* New connection: single client only */
        if (FD_ISSET(sockfd, &fds))
        {
            int fd = accept(sockfd, NULL, NULL);
            if (fd < 0)
            {
                if (errno != EINTR && errno != ECONNABORTED)
                    perror("accept");
            }
            else if (client_fd >= 0)
            {
                close(fd);
            }
            else
            {
                client_fd = fd;
                printf("Client connected.\n");
            }
        }

        /* PTY -> TCP */
        if (client_fd >= 0 && FD_ISSET(master_fd, &fds))
        {
            ssize_t n = read(master_fd, buf, BUF_SIZE);
            if (n > 0)
            {
                if (skip_echo)
                    skip_echo = 0;
                else if (write_all(client_fd, buf, (size_t)n) < 0)
                {
                    printf("Client write error.\n");
                    drop_client();
                }
            }
            else if (n < 0 && errno != EINTR && errno != EAGAIN)
            {
                perror("read(PTY)");
                break;
            }
            else if (n == 0)
            {
                printf("PTY closed.\n");
                break;
            }
        }

        /* TCP -> PTY */
        if (client_fd >= 0 && FD_ISSET(client_fd, &fds))
        {
            ssize_t n = read(client_fd, buf, BUF_SIZE);
            if (n > 0)
            {
                int do_exit = 0, do_quit = 0;
                scan_commands(buf, (int)n, &do_exit, &do_quit);

                if (write_all(master_fd, buf, (size_t)n) < 0)
                {
                    perror("write(PTY)");
                    break;
                }
                skip_echo = 1;

                if (do_quit)
                {
                    printf("User requested quit.\n");
                    stop_requested = 1;
                }
                else if (do_exit)
                {
                    printf("User requested exit. Restarting getty.\n");
                    restart_getty();
                }
            }
            else if (n == 0)
            {
                printf("Client disconnected.\n");
                drop_client();
            }
            else if (errno != EINTR && errno != EAGAIN)
            {
                perror("read(client)");
                drop_client();
            }
        }
    }

    cleanup();
    return 0;
}
