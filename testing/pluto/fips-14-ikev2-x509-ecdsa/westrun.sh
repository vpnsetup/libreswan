ipsec up west
../../guestbin/ping-once.sh --up -I 192.0.1.254 192.0.2.254
ipsec trafficstatus
ipsec listpubkeys
# test delete/free
ipsec delete west
echo done
