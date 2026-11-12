# TEST-ONLY Ed25519 fixture keys

These are **RFC 8032 test vectors**, not Edge catalog keys. They exist so Catch2 can
sign and verify plugin packages without a production private key.

**Do not ship these as trusted publishers.** `resources/python_plugins/trust_keys.json`
is empty on purpose. No real private key belongs in this repository.

## Test vector 1 (`ed25519_seed.hex` / `ed25519_public.hex`)

RFC 8032 test 1:

- seed (private scalar): `9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60`
- public key: `d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a`

## Test vector 2 (`ed25519_other_seed.hex` / `ed25519_other_public.hex`)

RFC 8032 test 2, used as the "wrong key" in signature-reject cases:

- seed: `4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb`
- public key: `3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c`
