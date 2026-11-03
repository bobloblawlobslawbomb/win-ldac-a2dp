# Third-party components

This project **does not redistribute** any third-party source or binaries. The build
fetches upstream code at pinned revisions and compiles it locally. The notices below
apply to the resulting binary.

---

## BTstack — BlueKitchen GmbH

<https://github.com/bluekitchen/btstack> · pinned at `9edb5d82`

BSD 3-Clause **with an additional non-commercial restriction**:

```
Copyright (C) 2009 BlueKitchen GmbH
All rights reserved

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. Neither the name of the copyright holders nor the names of contributors may
   be used to endorse or promote products derived from this software without
   specific prior written permission.
4. Any redistribution, use, or modification is done solely for personal
   benefit and not for any commercial purpose or for monetary gain.
```

**Clause 4 matters:** this software may be used and redistributed only for personal,
non-commercial purposes. Commercial use — including bundling it in a product or
selling it — requires a commercial licence from BlueKitchen GmbH
(contact@bluekitchen-gmbh.com).

---

## libldac — Sony / AOSP

<https://github.com/wdv4758h/libldac> (mirror of AOSP) · pinned at `8bd45f45`

Licensed under the **Apache License, Version 2.0**
(<http://www.apache.org/licenses/LICENSE-2.0>), including its patent grant.

See the upstream `NOTICE` file, which carries Sony's attribution requirements.

> **LDAC is a Sony trademark.** Sony requires LDAC *certification* to ship products that
> use it. Personal, non-commercial use does not require certification; distributing a
> product that advertises LDAC does.

---

## Development references

- [A2DP-Windows-Bridge](https://github.com/SeiyaFunaokaJP/A2DP-Windows-Bridge) (Apache-2.0/MIT)
  — demonstrated the WinUSB-claimed-radio approach.
- [Zadig](https://zadig.akeo.ie/) — WinUSB driver assignment; **not bundled**, users
  download it themselves.

---

## What is *not* used

No Microsoft Bluetooth audio driver is modified, patched, or redistributed. Windows'
inbox `BthA2dp.sys` is bypassed, not replaced — which is why no code signing,
attestation or EV certificate is involved.
