#pragma once
#include <stable-diffusion.h>
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <array>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace app_log {
struct Line { std::string time, text; int level; unsigned long long id; };
struct Store {
    std::mutex mutex;
    std::deque<Line> lines;
    unsigned long long revision = 1, next_id = 1;
};
inline Store& store() { static Store value; return value; }
inline unsigned long long append(std::string text, int level, unsigned long long replace = 0) {
    if (text.empty()) return 0;
    SYSTEMTIME time; GetLocalTime(&time);
    char stamp[16]; std::snprintf(stamp,sizeof(stamp),"%02u:%02u:%02u",time.wHour,time.wMinute,time.wSecond);
    auto& s=store(); std::scoped_lock lock(s.mutex);
    const auto id=s.next_id++;
    Line line{stamp,std::move(text),level,id};
    if(replace && !s.lines.empty() && s.lines.back().id==replace) s.lines.back()=std::move(line);
    else s.lines.push_back(std::move(line));
    while(s.lines.size()>4000)s.lines.pop_front();
    ++s.revision; return id;
}
// Handle split writes, terminal escape sequences and carriage-return progress lines.
struct Stream {
    std::string pending;
    int escape = 0, level = 2;
    unsigned long long replace = 0;
    bool carriage = false;
    void feed(const char* data, size_t count) {
        for(size_t i=0;i<count;++i) {
            const unsigned char c=data[i];
            if(escape) { if(escape==1) escape=c=='['?2:0; else if(c>=0x40 && c<=0x7e) escape=0; continue; }
            if(c==27){escape=1;continue;}
            if(c=='\r' || c=='\n') {
                if(!pending.empty()) { const auto id=append(std::move(pending),level,replace); pending.clear();replace=c=='\r'?id:0; }
                if(c=='\n')replace=0;
                carriage=c=='\r'; continue;
            }
            if(c<32 && c!='\t')continue;
            if(carriage)carriage=false;
            if(c=='\t')pending+="    ";else pending+=char(c);
            if(pending.size()>=4096){append(std::move(pending),level);pending.clear();replace=0;}
        }
    }
    void flush(){if(!pending.empty()){append(std::move(pending),level,replace);pending.clear();}}
};
inline void engine(sd_log_level_t level,const char* text,void*) noexcept {
    if(!text)return;
    try { Stream line;line.level=int(level);line.feed(text,std::char_traits<char>::length(text));line.flush(); }catch(...){}
}
// Lifetime surrounds App: workers stop before stream handles are restored.
class Capture {
    int old_out_=-1,old_err_=-1;
    HANDLE win_out_=nullptr,win_err_=nullptr;
    std::thread reader_;
public:
    Capture() {
        int pipe[2];
        if(_pipe(pipe,65536,_O_BINARY|_O_NOINHERIT)!=0)return;
        win_out_=GetStdHandle(STD_OUTPUT_HANDLE);win_err_=GetStdHandle(STD_ERROR_HANDLE);
        if(_fileno(stdout)<0){FILE* f=nullptr;freopen_s(&f,"NUL","w",stdout);}
        if(_fileno(stderr)<0){FILE* f=nullptr;freopen_s(&f,"NUL","w",stderr);}
        old_out_=_dup(_fileno(stdout));old_err_=_dup(_fileno(stderr));
        if(old_out_<0 || old_err_<0){if(old_out_>=0)_close(old_out_);if(old_err_>=0)_close(old_err_);old_out_=old_err_=-1;_close(pipe[0]);_close(pipe[1]);return;}
        const int read=pipe[0];
        reader_=std::thread([read]{
            Stream stream;
            std::array<char,4096> bytes;
            int count;
            while((count=_read(read,bytes.data(),unsigned(bytes.size())))>0) {
                try{stream.feed(bytes.data(),count);}catch(...){}
            }
            try{stream.flush();}catch(...){} _close(read);
        });
        std::fflush(stdout);std::fflush(stderr);
        _dup2(pipe[1],_fileno(stdout));_dup2(pipe[1],_fileno(stderr));_close(pipe[1]);
        setvbuf(stdout,nullptr,_IONBF,0);setvbuf(stderr,nullptr,_IONBF,0);
        SetStdHandle(STD_OUTPUT_HANDLE,reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout))));
        SetStdHandle(STD_ERROR_HANDLE,reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stderr))));
    }
    ~Capture(){
        if(!reader_.joinable())return;
        std::fflush(stdout);std::fflush(stderr);
        SetStdHandle(STD_OUTPUT_HANDLE,win_out_);SetStdHandle(STD_ERROR_HANDLE,win_err_);
        _dup2(old_out_,_fileno(stdout));_dup2(old_err_,_fileno(stderr));_close(old_out_);_close(old_err_);
        reader_.join();
    }
    Capture(const Capture&)=delete;
    Capture& operator=(const Capture&)=delete;
};
}
