ipsec up road

../../guestbin/ping-once.sh --up 192.0.2.254
ipsec whack --impair send_keepalive:1
../../guestbin/ping-once.sh --up 192.0.2.254

ipsec _kernel state
ipsec _kernel policy
echo done
