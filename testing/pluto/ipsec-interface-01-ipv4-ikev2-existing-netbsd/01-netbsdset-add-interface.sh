../../guestbin/prep.sh

ifconfig ipsec1 create
ifconfig ipsec1 -link2
ifconfig ipsec1 inet tunnel 198.18.1.15 198.18.1.12
ifconfig ipsec1 inet 198.18.15.15/24 198.18.12.12

ifconfig ipsec1
ipsec _kernel policy
