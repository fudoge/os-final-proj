#include <linux/limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>

#define BUF_SIZE 1024
#define MSG_SIZE (BUF_SIZE * 2)
#define NORMAL_SIZE 32

void *send_msg(void *arg);
void *recv_msg(void *arg);
void help();
int checkprefix(const char *str, const char *prefix);

char name[NORMAL_SIZE] = "[Unnamed]";
char server_ip[NORMAL_SIZE];
char server_port[NORMAL_SIZE];

int main(int argc, char *argv[]) {
    int sockfd, check;
    struct sockaddr_in serv_addr;
    pthread_t send_thread, recv_thead;
    void* thread_return;
    if(argc != 4) {
        fprintf(stderr, "Usage: %s <ip> <port> <name>\n", argv[0]);
        exit(1);
    }

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if(sockfd < 0) {
        perror("socket init error");
        exit(1);
    }

    snprintf(name, sizeof(name), "[%s]", argv[3]);
    snprintf(server_ip, sizeof(server_ip), "%s", argv[1]);
    snprintf(server_port, sizeof(server_port), "%s", argv[2]);

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(atoi(server_port));
    check = inet_pton(AF_INET, server_ip, &serv_addr.sin_addr);
    if(check < 0) {
        perror("inet_pton error");
        close(sockfd);
        exit(1);
    }

    printf("Connecting to Server(%s:%s) as %s...\n", server_ip, server_port, name);
    check = connect(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr));
    if(check < 0) {
        perror("connect error");
        close(sockfd);
        exit(1);
    }
    printf("Connected!\n");
    help();

    pthread_create(&send_thread, NULL, send_msg, (void*)&sockfd);
    pthread_create(&recv_thead, NULL, recv_msg, (void*)&sockfd);

    pthread_join(send_thread, &thread_return);
    pthread_join(recv_thead, &thread_return);
    close(sockfd);
    return 0;
}

void *send_msg(void *arg) {
    int sockfd = *((int *)arg);
    char name_msg[MSG_SIZE];
    char buf[BUF_SIZE];

    snprintf(name_msg, sizeof(name_msg), "[INFO] New User Connected: %s\n", name);
    write(sockfd, name_msg, strlen(name_msg));

    while(1) {
        fgets(buf, BUF_SIZE, stdin);
        if(strcmp(buf, "\n") == 0) continue;
        if(checkprefix(buf, "/setname")) {
            printf("Your new name is(max 31 byte(s)): ");
            fgets(buf, BUF_SIZE, stdin);
            buf[strcspn(buf, "\n")] = 0;

            char last_name[NAME_MAX];
            snprintf(last_name, sizeof(last_name), "%s", name);
            snprintf(name, sizeof(name), "[%s]", buf);
            printf("Changed successfully\n");

            // alert to room members
            snprintf(name_msg, sizeof(name_msg), "[INFO] Username Changed (%s -> %s)\n", last_name, name);
            write(sockfd, name_msg, strlen(name_msg));
            continue;
        }
        if(checkprefix(buf, "/whisper")) {
            // get target
            char target[NORMAL_SIZE];
            printf("Reciever name: ");
            fgets(target, sizeof(target), stdin);
            target[strcspn(target, "\n")] = 0;

            printf("Send Message to %s:\n", target);
            fgets(buf, sizeof(buf), stdin);

            snprintf(name_msg, sizeof(name_msg), "%s /whisper [%s] %s", name, target, buf);
            write(sockfd, name_msg, strlen(name_msg));
            continue;
        }
        if(checkprefix(buf, "/anonymous")) {
            char *body = strchr(buf, ' ');
            if(!body) continue;
            while(*body == ' ') body++;
            if(*body == '\n') continue;
            snprintf(name_msg, sizeof(name_msg), "[???] %s", body);
            write(sockfd, name_msg, strlen(name_msg));
            continue;
        }
        if(checkprefix(buf, "/quit")) {
            // alert to room members
            snprintf(name_msg, sizeof(name_msg), "[INFO] User Disconnected: %s\n", name);
            write(sockfd, name_msg, strlen(name_msg));
            printf("Disconnected\n");
            close(sockfd);
            exit(0);
        }
        if(checkprefix(buf, "/help")) {
            help();
            continue;
        }

        snprintf(name_msg, sizeof(name_msg), "%s %s", name, buf);
        write(sockfd, name_msg, strlen(name_msg));
    }
    return NULL;
}

void *recv_msg(void *arg) {
    int sockfd = *((int *)arg);
    char name_msg[NORMAL_SIZE + BUF_SIZE];

    while(1) {
        int n = read(sockfd, name_msg, NORMAL_SIZE + BUF_SIZE - 1);

        if(n < 0) {
            perror("recv error");
            return (void *)-1;
        } else if(n == 0) {
            break;
        }
        name_msg[n] = 0;

        char tmp[NAME_MAX + NORMAL_SIZE];
        strncpy(tmp, name_msg, sizeof(tmp));
        tmp[sizeof(tmp)-1] = '\0';

        char *p1 = strchr(tmp, ' ');
        if (!p1) {
            fputs(name_msg, stdout);
            continue;
        }
        *p1 = '\0';
        char *sender = tmp;

        char *p2 = strchr(p1+1, ' ');
        if (!p2) {
            fputs(name_msg, stdout);
            continue;
        }
        *p2 = '\0';
        char *cmd = p1+1;

        char *p3 = strchr(p2+1, ' ');
        if (!p3) {
            fputs(name_msg, stdout);
            continue;
        }
        *p3 = '\0';
        char *target = p2+1;

        char *message = p3+1;

        if (strcmp(cmd, "/whisper") == 0) {
            if(strcmp(target, name) == 0) {
                size_t L = strlen(message);
                if (L && message[L-1] == '\n') message[L-1] = '\0';

                printf("**Whisper from %s**: %s\n", sender, message);
            }
        } else {
            fputs(name_msg, stdout);
        }
    }


    return NULL;
}

void help() {
    printf("==========Chat Server Commands========\n");
    printf("/rooms            Show chatrooms with status\n");
    printf("/create           Create a new Chatroom. It can be ignored by server\n");
    printf("/join             Join Chatroom\n");
    printf("/delete           Delete Empty Chatroom\n");
    printf("/setname          New Nickname\n");
    printf("/whisper          Send private message\n");
    printf("/anonymous <msg>  Send anonymous message\n");
    printf("/quit             Quit Chat server\n");
    printf("/help             Show help messages\n");
}

int checkprefix(const char *str, const char *prefix) {
    return strncmp(str, prefix, strlen(prefix)) == 0;
}
