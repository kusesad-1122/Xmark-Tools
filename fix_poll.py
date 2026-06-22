#!/usr/bin/env python3
import os
os.chdir('/tmp/xmt')

with open('zygisk/zygisk_cpuinfo.cpp', 'r') as f:
    c = f.read()

# Fix the poll loop: when socket EOF, remove client fd from poll, only wait for inotify
# This prevents the infinite log-spamming loop that filled 25GB

old_poll = '''    bool app_died=false;
    while(!app_died){
        struct pollfd fds[2]; int nfds=0;
        fds[nfds].fd=client; fds[nfds].events=POLLIN; fds[nfds].revents=0; nfds++;
        if(inotify_fd>=0){fds[nfds].fd=inotify_fd;fds[nfds].events=POLLIN;fds[nfds].revents=0;nfds++;}
        int ret=poll(fds,nfds,-1);
        if(ret<0){if(errno==EINTR)continue;break;}
        if(fds[0].revents&(POLLIN|POLLHUP|POLLERR)){
            char buf[8]; ssize_t k=read(client,buf,sizeof(buf));
            if(k<=0){flog("SOCKET-EOF ignored (dlclose) nice=%s",nice);}
        }
        if(!app_died&&inotify_fd>=0&&(fds[1].revents&POLLIN)){
            char ev_buf[4096]; ssize_t len=read(inotify_fd,ev_buf,sizeof(ev_buf));
            if(len>0){flog("APP-DIED pid=%d (inotify)",app_pid);app_died=true;}
        }
    }'''

new_poll = '''    bool socket_eof=false;
    bool app_died=false;
    while(!app_died){
        struct pollfd fds[2]; int nfds=0;
        // Once socket EOFs (dlclose), remove from poll to prevent infinite loop
        if(!socket_eof){
            fds[nfds].fd=client; fds[nfds].events=POLLIN; fds[nfds].revents=0; nfds++;
        }
        if(inotify_fd>=0){
            fds[nfds].fd=inotify_fd; fds[nfds].events=POLLIN; fds[nfds].revents=0; nfds++;
        }
        // If both sockets are gone, nothing to poll - treat as death
        if(nfds==0){ app_died=true; break; }
        int ret=poll(fds,nfds,-1);
        if(ret<0){if(errno==EINTR)continue;break;}
        // Check client socket (only before EOF)
        if(!socket_eof && nfds>0 && (fds[0].revents&(POLLIN|POLLHUP|POLLERR))){
            char buf[8]; ssize_t k=read(client,buf,sizeof(buf));
            if(k<=0){
                socket_eof=true;
                flog("SOCKET-EOF (dlclose) nice=%s",nice);
                if(inotify_fd<0){ app_died=true; } // No inotify, socket EOF=death
            }
        }
        // Check inotify (index 0 or1 depending on socket_eof)
        if(!app_died&&inotify_fd>=0&&(fds[socket_eof?0:1].revents&POLLIN)){
            char ev_buf[4096]; ssize_t len=read(inotify_fd,ev_buf,sizeof(ev_buf));
            if(len>0){flog("APP-DIED pid=%d (inotify)",app_pid);app_died=true;}
        }
    }'''

if old_poll in c:
    c = c.replace(old_poll, new_poll, 1)
    print("Poll loop fixed: removed infinite logging")
else:
    print("ERROR: old poll pattern not found!")
    # Show what we have
    idx = c.find('bool app_died=false;')
    if idx>=0:
        print(f"Found 'bool app_died=false;' at {idx}")
        print(c[idx:idx+600])

with open('zygisk/zygisk_cpuinfo.cpp', 'w') as f:
    f.write(c)

print(f"File: {len(c)} bytes")