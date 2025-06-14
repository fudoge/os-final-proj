#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>
#include <errno.h>

#define PORT 8080
#define BUF_SIZE 1024
#define LOGBUF_SIZE (BUF_SIZE * 2)
#define QUEUE_SIZE (BUF_SIZE * 10)
#define NORMAL_SIZE 64
#define MAX_ROOMS 10
#define MAX_USER_EACH_ROOM 20

typedef struct Client Client;
typedef struct Room Room;
typedef struct RoomTable RoomTable;

typedef struct MessageQueue {
    char messages[BUF_SIZE*10];
    int head;
    int tail;
    pthread_mutex_t lock;
} MessageQueue;

struct Client {
    int client_fd;
    struct sockaddr_in client_addr;
    int room_idx;
    char recv_buf[BUF_SIZE];
    MessageQueue *mq;
    pthread_t tid;
};

struct Room {
    int num_clients;
    Client *users[MAX_USER_EACH_ROOM];
    pthread_mutex_t lock;
};

struct RoomTable {
    Room *rooms[MAX_ROOMS];
    pthread_mutex_t locks[MAX_ROOMS];
};

void handler(int signal);
void set_nonblocking(int fd);
int checkprefix(const char *str, const char *prefix);
void join_room(int room_idx, Client *c);
int create_room();
void delete_room(int room_idx);
void broadcast(Client *sender, Room *r, char *msg, size_t len);
int enqueue_msg(MessageQueue *mq, char *msg, size_t len);
void consume_msg(Client *c);
void *client_thread(void *arg);
void *log_thread(void *arg);
void get_timestamp(char *buf, size_t sz);
void log_print(char *msg);
void clear_resources();

RoomTable *rt;
MessageQueue *LogMQ;
pthread_t log_tid;
FILE *log_fp;
int log_run = 1;


int main() {
    int listenfd, sockfd, room_no;
    struct sockaddr_in serv_addr, client_addr;
    socklen_t client_len;

    signal(SIGINT, handler);

    rt = malloc(sizeof(RoomTable));
    for(int i = 0; i < MAX_ROOMS; i++) {
        rt->rooms[i] = NULL;
        pthread_mutex_init(&rt->locks[i], NULL);
    }
    room_no = create_room(); // initialize first(default) chatroom

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if(listenfd < 0) {
        perror("socket error");
        clear_resources();
        exit(1);
    }
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    if(bind(listenfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0){
        perror("bind error");
        clear_resources();
        exit(1);
    }
    if(listen(listenfd, 5) < 0) {
        perror("listen error");
        clear_resources();
        exit(1);
    }

    time_t now = time(NULL);
    struct tm *ltm = localtime(&now);
    char filename[NORMAL_SIZE];
    strftime(filename, sizeof(filename), "server_%Y%m%d_%H%M%S.log", ltm);
    log_fp = fopen(filename, "a");
    if(!log_fp) {
        perror("fopen error");
        clear_resources();
        exit(1);
    }

    LogMQ = malloc(sizeof(MessageQueue));
    pthread_mutex_init(&LogMQ->lock, NULL);
    LogMQ->head = 0;
    LogMQ->tail = 0;
    pthread_create(&log_tid, NULL, log_thread, NULL);

    while(1) {
        client_len = sizeof(client_addr);
        if((sockfd = accept(listenfd, (struct sockaddr *) &client_addr, &client_len)) < 0) {
            perror("accept error");
            continue;
        }

        // Init Client struct and make thread
        int client_success = 0;
        int room_idx = 0;
        Client *c = malloc(sizeof(Client));
        c->client_fd = sockfd;
        c->client_addr = client_addr;
        memset(c->recv_buf, 0, sizeof(c->recv_buf));
        c->mq = malloc(sizeof(MessageQueue));
        c->mq->head = 0;
        c->mq->tail = 0;
        memset(c->mq->messages, 0, sizeof(c->mq->messages));
        pthread_mutex_init(&c->mq->lock, NULL);
        for(int i = 0; i < MAX_ROOMS; i++) {
            pthread_mutex_lock(&rt->locks[i]);
            if(rt->rooms[i]->num_clients < MAX_USER_EACH_ROOM) {
                c->room_idx = i;
                room_idx = i;
                join_room(i, c);
                client_success = 1;
            }
            pthread_mutex_unlock(&rt->locks[i]);
            if(client_success) break;
        }
        if(!client_success) close(sockfd);

        // onboarding msg for new user
        time_t current_time;
        struct tm *local_time;
        time(&current_time);
        local_time = localtime(&current_time);

        char onboarding_buf[BUF_SIZE];
        snprintf(onboarding_buf, sizeof(onboarding_buf),
            "\n[Welcome To The Chat Server]\n"
            "Current Time: %04d-%02d-%02d %02d:%02d:%02d\n"
            "Current ChatRoom ID: %d\n\n",
            local_time->tm_year + 1900,
            local_time->tm_mon + 1,
            local_time->tm_mday,
            local_time->tm_hour,
            local_time->tm_min,
            local_time->tm_sec,
            room_idx
            );
        send(sockfd, onboarding_buf, sizeof(onboarding_buf), 0);

        if(client_success) {
            pthread_create(&c->tid, NULL, client_thread, c);
        }

    }
    return 0;
}

void handler(int signum) {
    printf("\nSIGNAL: Interrupt\nTerminating..\n");
    clear_resources();
    printf("All resoureces were cleared. bye.\n");
    exit(0);
}

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int checkprefix(const char *str, const char *prefix) {
    char *body = strchr(str, ']');
    if(body == NULL) return 0;
    body++;
    while(*body == ' ') body++;
    return strncmp(body, prefix, strlen(prefix)) == 0;
}

int create_room() {
    for(int i = 0; i < MAX_ROOMS; i++) {
        int flag = 0;
        pthread_mutex_lock(&rt->locks[i]);
        if(rt->rooms[i] == NULL) {
            Room *r = malloc(sizeof(Room));
            memset(r, 0, sizeof(Room));
            pthread_mutex_init(&r->lock, NULL);
            rt->rooms[i] = r;
            flag = 1;
        }
        pthread_mutex_unlock(&rt->locks[i]);
        if(flag) return i;
    }

    return -1;
}

void join_room(int room_idx, Client *c) {
    Room *r = rt->rooms[room_idx];
    pthread_mutex_lock(&r->lock);
    for(int i = 0; i < MAX_USER_EACH_ROOM; i++) {
        if(r->users[i] == NULL) {
            r->users[i] = c;
            r->num_clients++;
            break;
        }
    }
    pthread_mutex_unlock(&r->lock);
}

void leave_room(int room_idx, Client *c) {
    Room *r = rt->rooms[room_idx];
    pthread_mutex_lock(&r->lock);
    for(int i = 0; i < MAX_USER_EACH_ROOM; i++) {
        if(r->users[i] == c) {
            r->users[i] = NULL;
            r->num_clients--;
            break;
        }
    }
    pthread_mutex_unlock(&r->lock);
}

void delete_room(int room_idx) {
    pthread_mutex_lock(&rt->locks[room_idx]);

    Room *r = rt->rooms[room_idx];
    if (r) {
        pthread_mutex_destroy(&r->lock);
        free(r);
        rt->rooms[room_idx] = NULL;
    }

    pthread_mutex_unlock(&rt->locks[room_idx]);
}

void broadcast(Client *sender, Room *r, char *msg, size_t len) {
    for(int i = 0; i < MAX_USER_EACH_ROOM; i++) {
        Client *c = r->users[i];
        if(c && c != sender) {
            enqueue_msg(c->mq, msg, len);
        }
    }
}

int enqueue_msg(MessageQueue *mq, char *buf, size_t len) {
    pthread_mutex_lock(&mq->lock);
    int written = 0;
    for(size_t i = 0; i < len; i++) {
        int next = (mq->tail + 1) % QUEUE_SIZE;
        if(next == mq->head) break;
        mq->messages[mq->tail] = buf[i];
        mq->tail = next;
        written++;
    }
    pthread_mutex_unlock(&mq->lock);
    return written;
}

void consume_msg(Client *c) {
    MessageQueue *mq = c->mq;
    char send_buf[QUEUE_SIZE];
    int len = 0;
    pthread_mutex_lock(&mq->lock);
    while(mq->head != mq->tail) {
        send_buf[len++] = mq->messages[mq->head];
        mq->head = (mq->head + 1) % QUEUE_SIZE;
    }
    pthread_mutex_unlock(&mq->lock);

    if(len > 0) {
        int n = send(c->client_fd, send_buf, len, 0);
        if(n < 0) perror("send error");
    }
}

void* client_thread(void* arg) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    Client *c = (Client *) arg;
    int clientfd = c->client_fd;

    set_nonblocking(clientfd);

    while(1) {
        consume_msg(c);

        int n = recv(clientfd, c->recv_buf, BUF_SIZE, 0);
        if(n > 0) {
            if(checkprefix(c->recv_buf, "/rooms")) {
                char room_infos[BUF_SIZE] = "";
                int room_ids[MAX_ROOMS];
                int curr_users[MAX_ROOMS];
                int j = 0;
                for(int i = 0; i < MAX_ROOMS; i++) {
                    Room *r = rt->rooms[i];
                    if(r) {
                        pthread_mutex_lock(&r->lock);
                        room_ids[j] = i;
                        curr_users[j] = r->num_clients;
                        j++;
                        pthread_mutex_unlock(&r->lock);
                    }
                }
                char temp[BUF_SIZE];
                memset(temp, '\0', sizeof(temp));
                memset(room_infos, '\0', sizeof(room_infos));
                strcat(room_infos, "\n[Room Table]\n");
                for(int i = 0; i < j; i++) {
                    snprintf(temp, sizeof(temp), "Room %d: %d/%d\n", room_ids[i], curr_users[i], MAX_USER_EACH_ROOM);
                    strcat(room_infos, temp);
                }
                enqueue_msg(c->mq, room_infos, strlen(room_infos));
                continue;
            }
            if(checkprefix(c->recv_buf, "/create")) {
                int response = create_room();
                char buf[BUF_SIZE];
                char log_buf[LOGBUF_SIZE];
                if(response != -1) {
                    snprintf(buf, sizeof(buf), "New Room Created Successfully, ID: %d\n", response);
                    snprintf(log_buf, sizeof(log_buf), "%s by %d(%s:%d)", buf, c->client_fd, inet_ntoa(c->client_addr.sin_addr), ntohs(c->client_addr.sin_port));
                } else {
                    snprintf(buf, sizeof(buf), "Failed to Create Room.\n");
                }
                enqueue_msg(c->mq, buf, sizeof(buf));
                log_print(log_buf);
                continue;
            }
            if(checkprefix(c->recv_buf, "/join")) {
                // tokenize
                char *body = strchr(c->recv_buf, ']');
                if(!body) continue;
                body++;
                while(*body == ' ') body++;

                body += strlen("/join");
                while(*body == ' ') body++;

                int roomno = atoi(body);
                if(roomno < 0 || roomno >= MAX_ROOMS) {
                    char errmsg[BUF_SIZE];
                    snprintf(errmsg, sizeof(errmsg), "Invalid room ID: %d\n", roomno);
                    enqueue_msg(c->mq, errmsg, strlen(errmsg));
                    continue;
                }
                pthread_mutex_lock(&rt->locks[roomno]);
                Room *r = rt->rooms[roomno];
                pthread_mutex_unlock(&rt->locks[roomno]);
                if(!r) {
                    char errmsg[BUF_SIZE];
                    snprintf(errmsg, sizeof(errmsg), "Invalid room ID: %d\n", roomno);
                    enqueue_msg(c->mq, errmsg, strlen(errmsg));
                    continue;
                }
                leave_room(c->room_idx, c);
                int lastroom = c->room_idx;
                c->room_idx = roomno;
                join_room(roomno, c);

                char log_buf[LOGBUF_SIZE];
                snprintf(log_buf, sizeof(log_buf),
                    "User %d (%s:%d) moved from room %d to room %d\n",
                    c->client_fd,
                    inet_ntoa(c->client_addr.sin_addr),
                    ntohs(c->client_addr.sin_port),
                    lastroom,
                    c->room_idx
                );
                log_print(log_buf);

                char *sender = NULL;
                char *p = strchr(c->recv_buf, ']');
                p++;
                *p = '\0';
                sender = c->recv_buf;

                char out_msg[BUF_SIZE], in_msg[BUF_SIZE];
                snprintf(out_msg, sizeof(out_msg), "**Alert** %s moved from %d to %d\n", sender, lastroom, c->room_idx);
                snprintf(in_msg, sizeof(in_msg), "**Alert** %s moved from %d to %d\n", sender, lastroom, c->room_idx);
                broadcast(c, rt->rooms[lastroom], out_msg, sizeof(out_msg));
                broadcast(c, rt->rooms[c->room_idx], in_msg, sizeof(in_msg));

                char client_resp[BUF_SIZE];
                snprintf(client_resp, sizeof(client_resp), "**Alert** You moved from %d to %d successfully!\n", lastroom, c->room_idx);
                enqueue_msg(c->mq, client_resp, strlen(client_resp));

                continue;
            }
            if (checkprefix(c->recv_buf, "/delete")) {
                char *body = strchr(c->recv_buf, ']');
                if (!body) continue;
                body++;
                while (*body == ' ') body++;
                body += strlen("/delete");
                while (*body == ' ') body++;
                int roomno = atoi(body);
                if (roomno < 0 || roomno >= MAX_ROOMS) {
                    char errmsg[BUF_SIZE];
                    snprintf(errmsg, sizeof(errmsg), "Invalid room ID\n");
                    enqueue_msg(c->mq, errmsg, strlen(errmsg));
                    continue;
                }

                pthread_mutex_lock(&rt->locks[roomno]);
                Room *r = rt->rooms[roomno];
                if (!r) {
                    char errmsg[BUF_SIZE];
                    pthread_mutex_unlock(&rt->locks[roomno]);
                    snprintf(errmsg, sizeof(errmsg), "No such room: %d\n", roomno);
                    enqueue_msg(c->mq, errmsg, strlen(errmsg));
                    continue;
                }
                pthread_mutex_lock(&r->lock);
                if (r->num_clients > 0) {
                    pthread_mutex_unlock(&r->lock);
                    pthread_mutex_unlock(&rt->locks[roomno]);

                    char errmsg[BUF_SIZE];
                    snprintf(errmsg, sizeof(errmsg), "Cannot delete non empty room: %d\n", roomno);
                    enqueue_msg(c->mq, errmsg, strlen(errmsg));
                    continue;
                }
                rt->rooms[roomno] = NULL;
                pthread_mutex_unlock(&rt->locks[roomno]);

                pthread_mutex_unlock(&r->lock);
                delete_room(roomno);

                delete_room(roomno);
                char log_buf[LOGBUF_SIZE];
                snprintf(log_buf, sizeof(log_buf),
                    "User %d (%s:%d) deleted room %d",
                    c->client_fd,
                    inet_ntoa(c->client_addr.sin_addr),
                    ntohs(c->client_addr.sin_port),
                    roomno
                );
                log_print(log_buf);

                char client_resp[BUF_SIZE];
                snprintf(client_resp, sizeof(client_resp), "Room deleted successfully: %d\n", roomno);
                enqueue_msg(c->mq, client_resp, strlen(client_resp));
                continue;
            }

            char log_buf[LOGBUF_SIZE];
            snprintf(log_buf, sizeof(log_buf),
                "Message send from %d (%s:%d) in room %d: %s",
                c->client_fd,
                inet_ntoa(c->client_addr.sin_addr),
                ntohs(c->client_addr.sin_port),
                c->room_idx,
                c->recv_buf
            );
            log_print(log_buf);
            broadcast(c, rt->rooms[c->room_idx], c->recv_buf, n);
        } else if (n == 0) {
            char log_buf[LOGBUF_SIZE];
            snprintf(log_buf, sizeof(log_buf),
                "User %d (%s:%d) disconnected from room %d\n",
                c->client_fd,
                inet_ntoa(c->client_addr.sin_addr),
                ntohs(c->client_addr.sin_port),
                c->room_idx
            );
            log_print(log_buf);
            break;
        } else {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
            } else {
                perror("recv error");
                break;
            }
        }

        memset(c->recv_buf, 0, sizeof(c->recv_buf));
        usleep(100000);
    }

    leave_room(c->room_idx, c);
    return NULL;
}

void *log_thread(void *arg) {
    while(log_run) {
        usleep(100000);
        int len = 0;
        char buf[QUEUE_SIZE];
        memset(buf, 0, sizeof(buf));
        pthread_mutex_lock(&LogMQ->lock);
        while(LogMQ->head != LogMQ->tail && len < QUEUE_SIZE - 1) {
            buf[len++] = LogMQ->messages[LogMQ->head];
            LogMQ->head = (LogMQ->head + 1) % QUEUE_SIZE;
        }
        pthread_mutex_unlock(&LogMQ->lock);

        if(len > 0) {
            buf[len] = '\0';
            fprintf(log_fp, "%s", buf);
            fflush(log_fp);
        }
    }
    return NULL;
}

void get_timestamp(char *buf, size_t sz) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buf, sz, "%Y-%m-%d %H:%M:%S", tm_info);
}

void log_print(char *msg) {
    char buf[LOGBUF_SIZE];
    char ts[NORMAL_SIZE];
    get_timestamp(ts, sizeof(ts));

    snprintf(buf, sizeof(buf), "[%s] %s", ts, msg);
    enqueue_msg(LogMQ, buf, sizeof(buf));
}

void clear_resources() {
    for(int i = 0; i < MAX_ROOMS; i++) {
        Room *r = rt->rooms[i];
        if(!r) continue;
        for(int j = 0; j < MAX_USER_EACH_ROOM; j++) {
            Client *c = r->users[j];
            if(c) {
                shutdown(c->client_fd, SHUT_RDWR);
            }
        }
    }

    for(int i = 0; i < MAX_ROOMS; i++) {
        Room *r = rt->rooms[i];
        if(!r) continue;
        for(int j = 0; j < MAX_USER_EACH_ROOM; j++) {
            Client *c = r->users[j];
            if(!c) continue;

            void *ret;
            pthread_join(c->tid, &ret);

            close(c->client_fd);
            pthread_mutex_destroy(&c->mq->lock);
            free(c->mq);
            free(c);
            r->users[j] = NULL;
        }
        r->num_clients = 0;
    }

    for(int i = 0; i < MAX_ROOMS; i++) {
        pthread_mutex_lock(&rt->locks[i]);
        Room *r = rt->rooms[i];
        if(r) {
            pthread_mutex_destroy(&r->lock);
            free(r);
            rt->rooms[i] = NULL;
        }
        pthread_mutex_unlock(&rt->locks[i]);
        pthread_mutex_destroy(&rt->locks[i]);
    }
    free(rt);

    log_run = 0;
    pthread_join(log_tid, NULL);
    if(log_fp) {
        fclose(log_fp);
        log_fp = NULL;
    }
    pthread_mutex_destroy(&LogMQ->lock);
    free(LogMQ);
}
