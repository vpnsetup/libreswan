ipsec auto --up road-eastnet-nonat
ping -n -q -c 4 -I 192.0.2.219  192.0.2.254
ipsec whack --trafficstatus
../../guestbin/ipsec-kernel-state.sh
../../guestbin/ipsec-kernel-policy.sh
echo done
