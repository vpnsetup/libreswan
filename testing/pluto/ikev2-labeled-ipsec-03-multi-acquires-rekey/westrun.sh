# for port re-use in tests with protoport selectors
echo 1 >/proc/sys/net/ipv4/tcp_tw_reuse
ipsec auto --up labeled
# expect policy but no states
../../guestbin/ipsec-kernel-state.sh
../../guestbin/ipsec-kernel-policy.sh

# trigger an acquire; both ends initiate Child SA
echo "quit" | runcon -t netutils_t nc -w 50 -p 4301 -vvv 192.1.2.23 4300 2>&1 | sed "s/received in .*$/received .../"
../../guestbin/wait-for.sh --match 'labeled..2.' ipsec trafficstatus
# no shunts; two transports; two x two states
ipsec shuntstatus
ipsec showstates
../../guestbin/ipsec-kernel-state.sh
../../guestbin/ipsec-kernel-policy.sh

# let another on-demand label establish; only 1 SA is added
echo "quit" | runcon -u system_u -r system_r -t sshd_t nc -w 50 -vvv 192.1.2.23 22 2>&1 | sed "s/received in .*$/received .../"
../../guestbin/wait-for.sh --match 'labeled..3.' ipsec trafficstatus
# there should be no shunts
ipsec shuntstatus
ipsec showstates
../../guestbin/ipsec-kernel-state.sh
../../guestbin/ipsec-kernel-policy.sh

# now the fun begins
ipsec whack --rekey-ike --name 1
ipsec whack --rekey-ipsec --name 2
ipsec whack --rekey-ipsec --name 3
ipsec whack --rekey-ipsec --name 4

echo done
