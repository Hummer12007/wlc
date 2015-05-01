#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <chck/string/string.h>
#include "internal.h"
#include "tty.h"
#include "fd.h"

#if defined(__linux__)
#  include <linux/kd.h>
#  include <linux/major.h>
#  include <linux/vt.h>
#endif

#ifndef KDSKBMUTE
#  define KDSKBMUTE 0x4B51
#endif

static struct {
   struct {
      long kb_mode;
      int vt;
   } old_state;
   int tty, vt;
} wlc = {
   .old_state = {0},
   .tty = -1,
   .vt = 0,
};

static int
find_vt(const char *vt_string)
{
   if (vt_string) {
      int vt;
      if (chck_cstr_to_i32(vt_string, &vt)) {
         return vt;
      } else {
         wlc_log(WLC_LOG_WARN, "Invalid vt '%s', trying to find free vt", vt_string);
      }
   }

   int tty0_fd;
   if ((tty0_fd = open("/dev/tty0", O_RDWR | O_CLOEXEC)) < 0)
      die("Could not open /dev/tty0 to find free vt");

   int vt;
   if (ioctl(tty0_fd, VT_OPENQRY, &vt) != 0)
      die("Could not find free vt");

   close(tty0_fd);
   return vt;
}

static int
open_tty(int vt)
{
   char tty_name[64];
   snprintf(tty_name, sizeof tty_name, "/dev/tty%d", vt);

   /* check if we are running on the desired vt */
   if (ttyname(STDIN_FILENO) && chck_cstreq(tty_name, ttyname(STDIN_FILENO)))
      return STDIN_FILENO;

   int fd;
   if ((fd = open(tty_name, O_RDWR | O_NOCTTY | O_CLOEXEC)) < 0)
      die("Could not open %s", tty_name);

   wlc_log(WLC_LOG_INFO, "Running on vt %d", vt);
   return fd;
}

static bool
setup_tty(int fd, bool replace_vt)
{
   if (fd < 0)
      return false;

   struct stat st;
   if (fstat(fd, &st) == -1)
      die("Could not stat tty fd");

   wlc.vt = minor(st.st_rdev);

   if (major(st.st_rdev) != TTY_MAJOR || wlc.vt == 0)
      die("Not a valid vt");

   if (!replace_vt) {
      int kd_mode;
      if (ioctl(fd, KDGETMODE, &kd_mode) == -1)
         die("Could not get vt%d mode", wlc.vt);

      if (kd_mode != KD_TEXT)
         die("vt%d is already in graphics mode (%d). Is another display server running?", wlc.vt, kd_mode);
   }

   struct vt_stat state;
   if (ioctl(fd, VT_GETSTATE, &state) == -1)
      die("Could not get current vt");

   wlc.old_state.vt = state.v_active;

   if (ioctl(fd, VT_ACTIVATE, wlc.vt) == -1)
      die("Could not activate vt%d", wlc.vt);

   if (ioctl(fd, VT_WAITACTIVE, wlc.vt) == -1)
      die("Could not wait for vt%d to become active", wlc.vt);

   if (ioctl(fd, KDGKBMODE, &wlc.old_state.kb_mode))
      die("Could not get keyboard mode");

   // vt will be restored from now on
   wlc.tty = fd;

   if (ioctl(fd, KDSKBMUTE, 1) == -1 && ioctl(fd, KDSKBMODE, K_OFF) == -1) {
      wlc_tty_terminate();
      die("Could not set keyboard mode to K_OFF");
   }

   if (ioctl(fd, KDSETMODE, KD_GRAPHICS) == -1) {
      wlc_tty_terminate();
      die("Could not set console mode to KD_GRAPHICS");
   }

   struct vt_mode mode = {
      .mode = VT_PROCESS,
      .relsig = SIGUSR1,
      .acqsig = SIGUSR2
   };

   if (ioctl(fd, VT_SETMODE, &mode) == -1) {
      wlc_tty_terminate();
      die("Could not set vt%d mode", wlc.vt);
   }

   return true;
}

static void
sigusr_handler(int signal)
{
   switch (signal) {
      case SIGUSR1:
         wlc_log(WLC_LOG_INFO, "SIGUSR1");
         wlc_set_active(false);
         break;
      case SIGUSR2:
         wlc_log(WLC_LOG_INFO, "SIGUSR2");
         wlc_set_active(true);
         break;
   }
}

void
wlc_tty_activate(void)
{
   if (!wlc_fd_activate()) {
      die("Failed to activate tty");
      return;
   }

   wlc_log(WLC_LOG_INFO, "Activating tty");
   ioctl(wlc.tty, VT_RELDISP, VT_ACKACQ);
}

void
wlc_tty_deactivate(void)
{
   if (!wlc_fd_deactivate()) {
      die("Failed to release tty");
      return;
   }

   wlc_log(WLC_LOG_INFO, "Releasing tty");
   ioctl(wlc.tty, VT_RELDISP, 1);
}

bool
wlc_tty_activate_vt(int vt)
{
   if (wlc.tty < 0 || vt == wlc.vt)
      return false;

   wlc_log(WLC_LOG_INFO, "Activate vt: %d", vt);
   return (ioctl(wlc.tty, VT_ACTIVATE, vt) != -1);
}

int
wlc_tty_get_vt(void)
{
   return wlc.vt;
}

void
wlc_tty_terminate(void)
{
   if (wlc.tty >= 0) {
      wlc_log(WLC_LOG_INFO, "Restoring tty %d (0x%lx)", wlc.tty, wlc.old_state.kb_mode);
      // The ACTIVATE / WAITACTIVE may be potentially bad here.
      // However, we need to make sure the vt we initially opened is also active on cleanup.
      // We can't make sure this is synchronized due to unclean exits.
      ioctl(wlc.tty, VT_ACTIVATE, wlc.vt);
      ioctl(wlc.tty, VT_WAITACTIVE, wlc.vt);
      ioctl(wlc.tty, KDSKBMUTE, 0);
      ioctl(wlc.tty, KDSKBMODE, wlc.old_state.kb_mode);
      ioctl(wlc.tty, KDSETMODE, KD_TEXT);
      struct vt_mode mode = { .mode = VT_AUTO };
      ioctl(wlc.tty, VT_SETMODE, &mode);
      ioctl(wlc.tty, VT_ACTIVATE, wlc.old_state.vt);
      close(wlc.tty);
   }

   memset(&wlc, 0, sizeof(wlc));
   wlc.tty = -1;
}

void
wlc_tty_init(int vt)
{
   if (wlc.tty >= 0)
      return;

   const bool replace_vt = (vt > 0);

   if (!vt && !(vt = find_vt(getenv("XDG_VTNR"))))
      die("Could not find vt");

   if (!setup_tty(open_tty(vt), replace_vt))
      die("Could not open tty with vt%d", vt);

   struct sigaction action = {
      .sa_handler = sigusr_handler
   };

   sigaction(SIGUSR1, &action, NULL);
   sigaction(SIGUSR2, &action, NULL);
}
