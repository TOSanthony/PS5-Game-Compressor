#!/usr/bin/env python3
"""Emits a C header with the CA bundle (PEM) used by the self-update TLS client."""
import json, sys
pem = open(sys.argv[1], encoding="ascii", errors="ignore").read()
print("static const char gc_ca_bundle[] = " + json.dumps(pem) + ";")
