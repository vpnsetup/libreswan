/testing/guestbin/swan-prep --nokeys
/testing/x509/import.sh real/mainca/`hostname`.all.p12

ipsec start
../../guestbin/wait-until-pluto-started
ipsec add road-east
ipsec whack --impair revival
echo initdone
