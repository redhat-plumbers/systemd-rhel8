# This file is part of systemd.
#
# Used by systemd --user instances.

account sufficient pam_unix.so no_pass_expiry
m4_ifdef(`HAVE_SELINUX',
session required pam_selinux.so close
session required pam_selinux.so nottys open
)m4_dnl
session required pam_loginuid.so
session optional pam_keyinit.so force revoke
session required pam_namespace.so
session optional pam_umask.so silent
session optional pam_systemd.so
