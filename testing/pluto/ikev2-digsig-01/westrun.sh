# expected to fail
#ipsec whack --impair delete-on-retransmit
ipsec auto --up  westnet-eastnet-ikev2
#ping -n -c4 -I 192.0.1.254 192.0.2.254
ipsec whack --trafficstatus
echo done
