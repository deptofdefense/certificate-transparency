#include <deque>
#include <iostream>

#include <assert.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>

#define VV(x)

class EventLoop;

class Services {
public:

  // because time is expensive, for most tasks we can just use some
  // time sampled within this event handling loop. So, the main loop
  // needs to call SetRoughTime() appropriately.
  static time_t RoughTime() {
    if (roughTime_ == 0)
      roughTime_ = time(NULL);
    return roughTime_;
  }

  static void SetRoughTime() { roughTime_ = 0; }

private:

  static time_t roughTime_;
};

time_t Services::roughTime_;

class FD {
public:

  enum CanDelete {
    DELETE,
    NO_DELETE
  };

  FD(EventLoop *loop, int fd, CanDelete deletable = DELETE);

  virtual bool WantsWrite() const = 0;

  virtual void WriteIsAllowed() = 0;

  virtual bool WantsRead() const = 0;

  virtual void ReadIsAllowed() = 0;

  bool WantsErase() const { return wantsErase_; }

  void Close() {
    assert(deletable_ == DELETE);
    if (wantsErase_)
	{
	std::cout << "Already closed " << fd() << std::endl;
	return;
	}
    std::cout << "Closing " << fd() << std::endl;
    wantsErase_ = true;
    shutdown(fd(), SHUT_RDWR);
    close(fd());
  }

  int fd() const { return fd_; }

  bool CanDrop() const { return deletable_ == DELETE; }

  // Don't forget to call me if anything happens!
  // FIXME: time() is expensive - just a serial number instead?
  void Activity() { lastActivity_ = Services::RoughTime(); }

  time_t LastActivity() const { return lastActivity_; }

protected:

  EventLoop *loop() const { return loop_; }

  bool WillAccept(int fd);

private:

  int fd_;
  EventLoop *loop_;
  bool wantsErase_;
  CanDelete deletable_;
  time_t lastActivity_;

  // Note that while you can set these low for test, they behave a
  // bit strangely when set low - for example, it is quite easy to
  // hit the limit even if the window is not 0. I'm guessing 1000
  // and 100 would be good numbers. Note EventLoop::idleTime_ below,
  // also.
  static const int fdLimit_ = 1000;
  static const int fdLimitWindow_ = 1;
};

class Listener : public FD {
public:

  Listener(EventLoop *loop, int fd) : FD(loop, fd, NO_DELETE) {}

  bool WantsRead() const { return true; }

  void ReadIsAllowed() {
    int in = accept(fd(), NULL, NULL);
    assert(in >= 0);
    if (!WillAccept(in)) {
      static char sorry[] = "You're too big.\n";
    
      write(in, sorry, sizeof sorry);
      shutdown(in, SHUT_RDWR);
      close(in);
      return;
    }
    Accepted(in);
  }

  bool WantsWrite() const { return false; }

  void WriteIsAllowed() { assert(false); }

  virtual void Accepted(int fd) = 0;
};

class EventLoop {
public:

  void Add(FD *fd) { fds_.push_back(fd); }

  void OneLoop() {
    fd_set readers, writers;
    int max = -1;

    memset(&readers, '\0', sizeof readers);
    memset(&writers, '\0', sizeof writers);
    for (std::deque<FD *>::const_iterator pfd = fds_.begin();
	 pfd != fds_.end(); ++pfd) {
      FD *fd = *pfd;

      assert(!fd->WantsErase());
      if (fd->WantsWrite())
	Set(fd->fd(), &writers, &max);
      if (fd->WantsRead())
	Set(fd->fd(), &readers, &max);
    }

    assert(max >= 0);
    VV(std::cout << "Before select" << std::endl);
    int r = select(max+1, &readers, &writers, NULL, NULL);
    VV(std::cout << "After select" << std::endl);
    assert(r > 0);

    Services::SetRoughTime();
    int n = 0;
    for (std::deque<FD *>::iterator pfd = fds_.begin(); pfd != fds_.end(); ) {
      FD *fd = *pfd;

      if (EraseCheck(&pfd))
	  continue;

      if (FD_ISSET(fd->fd(), &writers)) {
	assert(fd->WantsWrite());
	fd->WriteIsAllowed();
	fd->Activity();
	++n;
      }

      if (EraseCheck(&pfd))
	  continue;

      if (FD_ISSET(fd->fd(), &readers))	{
	assert(fd->WantsRead());
	fd->ReadIsAllowed();
	fd->Activity();
	++n;
      }

      if (EraseCheck(&pfd))
	  continue;

      ++pfd;
    }
    assert(n <= r);
  }

  void Forever() {
    for ( ; ; )
      OneLoop();
  }

  void MaybeDropOne() {
    std::deque<FD *>::iterator drop = fds_.end();
    time_t oldest = Services::RoughTime() - idleTime_;

    for (std::deque<FD *>::iterator pfd = fds_.begin();
	 pfd != fds_.end(); ++pfd) {
      FD *fd = *pfd;

      if (fd->CanDrop() && fd->LastActivity() < oldest) {
	oldest = fd->LastActivity();
	drop = pfd;
      }
    }
    if (drop != fds_.end())
	(*drop)->Close();
  }

private:

  bool EraseCheck(std::deque<FD *>::iterator *pfd) {
    if ((**pfd)->WantsErase()) {
      *pfd = fds_.erase(*pfd);
      delete **pfd;
      return true;
    }
    return false;
  }

  static void Set(int fd, fd_set *fdset, int *max) {
    assert(fd >= 0);
    assert((unsigned)fd < FD_SETSIZE);
    FD_SET(fd, fdset);
    if (fd > *max)
      *max = fd;
  }

  std::deque<FD *> fds_;
  time_t roughTime_;
  // This should probably be set to 2 for anything but test (or 1 or 0).
  // 2: everything gets a chance to speak.
  // 1: sometimes the clock will tick before some get a chance to speak.
  // 0: maybe no-one ever gets a chance to speak.
  static const time_t idleTime_ = 20;
};

FD::FD(EventLoop *loop, int fd, CanDelete deletable)
  : fd_(fd), loop_(loop), wantsErase_(false), deletable_(deletable) {
  assert(fd >= 0);
  assert((unsigned)fd < FD_SETSIZE);
  loop->Add(this);
  Activity();
}

bool FD::WillAccept(int fd) {
  if (fd >= fdLimit_ - fdLimitWindow_)
    loop()->MaybeDropOne();
  return fd < fdLimit_;
}

class Server : public FD {
public:

  Server(EventLoop *loop, int fd) : FD(loop, fd) {}

  bool WantsRead() const { return true; }

  void ReadIsAllowed() {
    char buf[1024];

    ssize_t n = read(fd(), buf, sizeof buf);
    VV(std::cout << "read " << n << " from " << fd() << std::endl);
    if (n <= 0)	{
      Close();
      return;
    }
    rbuffer_.append(buf, (size_t)n);
    BytesRead(&rbuffer_);
  }

  // There are fresh bytes available in rbuffer.  It is the callee's
  // responsibility to remove consumed bytes from rbuffer. This will
  // NOT be called again until more data arrives from the network,
  // even if there are unconsumed bytes in rbuffer.
  virtual void BytesRead(std::string *rbuffer) = 0;

  bool WantsWrite() const { return !wbuffer_.empty(); }

  void WriteIsAllowed() {
    int n = write(fd(), wbuffer_.data(), wbuffer_.length());
    VV(std::cout << "wrote " << n << " to " << fd() << std::endl);
    if (n <= 0) {
      Close();
      return;
    }
    wbuffer_.erase(0, n);
  }

  void Write(std::string str) {	wbuffer_.append(str); }

private:

  std::string rbuffer_;
  std::string wbuffer_;
};

class LineServer : public Server {
public:

  LineServer(EventLoop *loop, int fd) : Server(loop, fd) {}

  void BytesRead(std::string *rbuffer) {
    for ( ; ; ) {
      size_t end;
      end = rbuffer->find('\n');
      if (end == std::string::npos)
	break;
      size_t use = end;
      if (use > 0 && (*rbuffer)[use - 1] == '\r')
	--use;
      LineRead(rbuffer->substr(0, use));
      rbuffer->erase(0, end+1);
    }
  }

  // A line has been read from the network. The newline has been
  // removed.
  virtual void LineRead(const std::string &line) = 0;
};

class EchoServer : public LineServer {
public:

  EchoServer(EventLoop *loop, int fd) : LineServer(loop, fd) {}

  void LineRead(const std::string &line) {
    Write(line);
    Write("\n");
  }
};

class EchoListener : public Listener {
public:

  EchoListener(EventLoop *loop, int fd) : Listener(loop, fd) {}

  void Accepted(int fd)	{
    std::cout << "Accepted " << fd << std::endl;
    new EchoServer(loop(), fd);
  }
};

static bool init_server(int *sock, int port, const char *ip, int type) {
  bool ret = false;
  struct sockaddr_in server;
  int s = -1;

  memset(&server, 0, sizeof(server));
  server.sin_family = AF_INET;
  server.sin_port = htons((unsigned short)port);
  if (ip == NULL)
    server.sin_addr.s_addr = INADDR_ANY;
  else
    memcpy(&server.sin_addr.s_addr, ip, 4);
  
  if (type == SOCK_STREAM)
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  else /* type == SOCK_DGRAM */
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == -1)
    goto err;

  {
    int j = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &j, sizeof j);
  }

  if (bind(s, (struct sockaddr *)&server, sizeof(server)) == -1) {
    perror("bind");
    goto err;
  }
  /* Make it 128 for linux */
  if (type == SOCK_STREAM && listen(s, 128) == -1) goto err;
  *sock = s;
  ret = true;
 err:
  if (!ret && s != -1) {
    shutdown(s, SHUT_RDWR);
    close(s);
  }
  return ret;
}

int main(int argc, char **argv) {
  int port = atoi(argv[1]);

  int fd;
  assert(init_server(&fd, port, NULL, SOCK_STREAM));

  EventLoop loop;
  EchoListener l(&loop, fd);
  loop.Forever();
}