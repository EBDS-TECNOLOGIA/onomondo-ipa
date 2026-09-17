#!/bin/sh
# Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-only
#
# The eUICC trust anchors (trustedCertificateTls, trustedEimPkTls) against real TLS handshakes, with whichever
# TLS library the build uses (http_tls_openssl.c or http_tls_mbedtls.c): the same cases must give the same
# results with both.
#
# usage: http_tls_test.sh CLIENT_BINARY
# Needs openssl (the command) and python3.

set -u
CLIENT=$1
HERE=$(dirname "$0")
T=$(mktemp -d /tmp/http_tls_test_XXXXXX)
PIDS=
trap 'kill $PIDS 2>/dev/null; rm -rf "$T"' EXIT
failures=0

cd "$T"

# --- PKI --------------------------------------------------------------------------------------------------------
key() { openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$1.key" 2>/dev/null; }
ext() { printf '%s\n' "$@" > ext.cnf; }

# self-signed root: NAME
root() {
	key "$1"
	openssl req -x509 -new -key "$1.key" -subj "/CN=$1" -days 30 -out "$1.pem" \
		-addext basicConstraints=critical,CA:TRUE -addext keyUsage=critical,keyCertSign 2>/dev/null
}
# certificate: NAME ISSUER KIND(ca|leaf) [DAYS]
cert() {
	key "$1"
	openssl req -new -key "$1.key" -subj "/CN=$1" -out "$1.csr" 2>/dev/null
	if [ "$3" = ca ]; then
		ext "basicConstraints=critical,CA:TRUE" "keyUsage=critical,keyCertSign"
	else
		ext "basicConstraints=critical,CA:FALSE" "subjectAltName=DNS:localhost" "keyUsage=critical,digitalSignature" \
			"extendedKeyUsage=serverAuth"
	fi
	openssl x509 -req -in "$1.csr" -CA "$2.pem" -CAkey "$2.key" -CAcreateserial -days "${4:-30}" \
		-extfile ext.cnf -out "$1.pem" 2>/dev/null
}
der() { openssl x509 -in "$1.pem" -outform DER -out "$1.der"; }
spki() { openssl pkey -in "$1.key" -pubout -outform DER -out "$1.spki" 2>/dev/null; }

root rootA
root rootB
cert interA rootA ca
cert leaf rootA leaf
cert leafI interA leaf
cert leafI2 interA leaf
# An expired certificate: backdated with a minimal CA configuration, as "openssl x509" cannot.
key expired
openssl req -new -key expired.key -subj "/CN=expired" -out expired.csr 2>/dev/null
mkdir -p ca && : > ca/index.txt && echo 01 > ca/serial
cat > ca.cnf <<CNF
[ca]
default_ca = d
[d]
dir = ca
database = ca/index.txt
serial = ca/serial
new_certs_dir = ca
default_md = sha256
policy = p
copy_extensions = none
x509_extensions = x
[p]
commonName = supplied
[x]
basicConstraints = critical,CA:FALSE
subjectAltName = DNS:localhost
CNF
openssl ca -batch -config ca.cnf -cert rootA.pem -keyfile rootA.key -in expired.csr -out expired.pem \
	-startdate 20200101000000Z -enddate 20210101000000Z >/dev/null 2>&1 || { echo "cannot create expired cert"; exit 1; }
# self-signed server certificate
key self
openssl req -x509 -new -key self.key -subj "/CN=localhost" -days 30 -out self.pem \
	-addext subjectAltName=DNS:localhost -addext basicConstraints=critical,CA:FALSE 2>/dev/null

for n in rootA rootB interA leaf leafI leafI2 self; do der $n; done
for n in rootA rootB interA self; do spki $n; done

# --- servers ----------------------------------------------------------------------------------------------------
# server NAME KEY CERT...: the chain is presented in the order given
server() {
	name=$1
	k=$2
	shift 2
	cat "$@" > "$name.chain"
	python3 "$HERE/https_server.py" "$name.chain" "$k" "$name.port" &
	PIDS="$PIDS $!"
}
server s_root leaf.key leaf.pem rootA.pem
server s_inter leafI.key leafI.pem interA.pem
server s_full leafI.key leafI.pem interA.pem rootA.pem
server s_expired expired.key expired.pem rootA.pem
server s_self self.key self.pem

for s in s_root s_inter s_full s_expired s_self; do
	i=0
	while [ ! -s "$s.port" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
	[ -s "$s.port" ] || { echo "server $s did not start"; exit 1; }
done
url() { echo "https://localhost:$(cat "$1.port")/"; }

# case NAME SERVER EXPECTED STEP...
case_() {
	name=$1
	srv=$2
	expected=$3
	shift 3
	got=$("$CLIENT" "$(url "$srv")" "$@" 2>"$T/client.err" | sed 's/ *$//')
	if [ "$got" = "$expected" ]; then
		echo "PASS: $name"
	else
		echo "FAIL: $name (expected '$expected', got '$got')"
		sed 's/^/    /' "$T/client.err"
		failures=$((failures + 1))
	fi
}

case_ "no anchor: private root is not trusted" s_root FAIL req
case_ "CA bundle file" s_root OK cabundle:rootA.pem req
case_ "trustedCertificateTls = root" s_root OK ca-der:rootA.der req
case_ "trustedCertificateTls = other root" s_root FAIL ca-der:rootB.der req
case_ "trustedEimPkTls = root key" s_root OK ca-spki:rootA.spki req
case_ "trustedEimPkTls = other key" s_root FAIL ca-spki:rootB.spki req
case_ "verification off ignores a wrong anchor" s_root OK noverif ca-spki:rootB.spki req
case_ "expired server certificate, certificate anchor" s_expired FAIL ca-der:rootA.der req
case_ "expired server certificate, key anchor" s_expired FAIL ca-spki:rootA.spki req
case_ "intermediate as trustedCertificateTls" s_inter OK ca-der:interA.der req
case_ "intermediate key as trustedEimPkTls" s_inter OK ca-spki:interA.spki req
case_ "root certificate completes a chain sent without it" s_inter OK ca-der:rootA.der req
case_ "root key needs the root in the chain (documented limitation)" s_inter FAIL ca-spki:rootA.spki req
case_ "root key with the root in the chain" s_full OK ca-spki:rootA.spki req
case_ "self-signed server pinned by certificate" s_self OK ca-der:self.der req
case_ "self-signed server pinned by key" s_self OK ca-spki:self.spki req
case_ "self-signed server, other anchor" s_self FAIL ca-der:rootA.der req
case_ "server certificate pinned, chain sent along" s_full OK ca-der:leafI.der req
case_ "other certificate of the same CA pinned" s_full FAIL ca-der:leafI2.der req
case_ "replacing the anchor applies to the next request" s_root "OK FAIL OK" \
	ca-der:rootA.der req ca-der:rootB.der req ca-der:rootA.der req
case_ "key anchor replaced by another key" s_root "OK FAIL" ca-spki:rootA.spki req ca-spki:rootB.spki req
case_ "garbage is refused as anchor" s_root "BADANCHOR FAIL" ca-der:rootA.key req

# Host name check still applies with an eUICC anchor.
url() { echo "https://127.0.0.1:$(cat "$1.port")/"; }
case_ "wrong host name, certificate anchor" s_root FAIL ca-der:rootA.der req
case_ "wrong host name, key anchor" s_root FAIL ca-spki:rootA.spki req

echo "$failures failure(s)"
[ "$failures" -eq 0 ]
