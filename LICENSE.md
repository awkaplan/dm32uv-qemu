# License

Required Notice: Copyright 2026 Alex Kaplan (https://github.com/awkaplan/dm32uv-qemu)

Except for the material listed under "What this license does not cover" below, the contents of
this repository are licensed under the **PolyForm Noncommercial License 1.0.0**, reproduced in
full at the end of this file. These are the same terms as the
[dm32uv-re](https://github.com/awkaplan/dm32uv-re) project this board model was written for. Under
the license's "Notices" section, anyone who passes on any part of this material must also pass on
these terms (or their URL) and the `Required Notice:` line above.

Every source file states its terms in an `SPDX-License-Identifier` line at the top.

## What this license does not cover

The licensor can license only what the licensor owns. The following keeps its own terms:

1. **Files derived from csky-qemu.** `hw/csky/Kconfig` and `hw/csky/meson.build` are
   [csky-qemu](https://github.com/awkaplan/csky-qemu)'s files with the `dm32` machine added. They
   stay under csky-qemu's terms, `LGPL-2.1-or-later` ([`COPYING.LIB`](COPYING.LIB)), as their
   `SPDX-License-Identifier` lines say.
2. **`integration/top-level.diff`.** It is a patch to QEMU's own build files, and each hunk is
   under the license of the QEMU file it changes (see [`COPYING`](COPYING) and
   [`COPYING.LIB`](COPYING.LIB), copied unchanged from QEMU).
3. **QEMU and csky-qemu.** This repository includes neither. You need your own QEMU checkout, under
   QEMU's terms, and csky-qemu, under its own `LICENSE.md`, to build it.
4. **Firmware and vendor material.** No radio firmware, boot ROM, codeplug, calibration data or
   datasheet is included. The device models were written from the HR_C7000 and FD6818 datasheets
   and from observing the stock firmware's register traffic; they contain no vendor code.
5. **Trademarks.** Baofeng and DM-32UV are names of their owners, used here only to say which radio
   the model emulates. This project is not affiliated with or endorsed by Baofeng.

## Building, using and distributing binaries

QEMU is licensed under the GNU General Public License, version 2, which does not allow a combined
work to be distributed under further restrictions such as a noncommercial condition. So:

- You may build QEMU with this overlay and run it, for any noncommercial purpose these terms permit.
- Do not distribute `qemu-system-cskyv2` binaries, container images or other builds that include
  this overlay. Share the source and let others build it.

---

# PolyForm Noncommercial License 1.0.0

<https://polyformproject.org/licenses/noncommercial/1.0.0>

## Acceptance

In order to get any license under these terms, you must agree
to them as both strict obligations and conditions to all
your licenses.

## Copyright License

The licensor grants you a copyright license for the
software to do everything you might do with the software
that would otherwise infringe the licensor's copyright
in it for any permitted purpose.  However, you may
only distribute the software according to [Distribution
License](#distribution-license) and make changes or new works
based on the software according to [Changes and New Works
License](#changes-and-new-works-license).

## Distribution License

The licensor grants you an additional copyright license
to distribute copies of the software.  Your license
to distribute covers distributing the software with
changes and new works permitted by [Changes and New Works
License](#changes-and-new-works-license).

## Notices

You must ensure that anyone who gets a copy of any part of
the software from you also gets a copy of these terms or the
URL for them above, as well as copies of any plain-text lines
beginning with `Required Notice:` that the licensor provided
with the software.  For example:

> Required Notice: Copyright Yoyodyne, Inc. (http://example.com)

## Changes and New Works License

The licensor grants you an additional copyright license to
make changes and new works based on the software for any
permitted purpose.

## Patent License

The licensor grants you a patent license for the software that
covers patent claims the licensor can license, or becomes able
to license, that you would infringe by using the software.

## Noncommercial Purposes

Any noncommercial purpose is a permitted purpose.

## Personal Uses

Personal use for research, experiment, and testing for
the benefit of public knowledge, personal study, private
entertainment, hobby projects, amateur pursuits, or religious
observance, without any anticipated commercial application,
is use for a permitted purpose.

## Noncommercial Organizations

Use by any charitable organization, educational institution,
public research organization, public safety or health
organization, environmental protection organization,
or government institution is use for a permitted purpose
regardless of the source of funding or obligations resulting
from the funding.

## Fair Use

You may have "fair use" rights for the software under the
law. These terms do not limit them.

## No Other Rights

These terms do not allow you to sublicense or transfer any of
your licenses to anyone else, or prevent the licensor from
granting licenses to anyone else.  These terms do not imply
any other licenses.

## Patent Defense

If you make any written claim that the software infringes or
contributes to infringement of any patent, your patent license
for the software granted under these terms ends immediately. If
your company makes such a claim, your patent license ends
immediately for work on behalf of your company.

## Violations

The first time you are notified in writing that you have
violated any of these terms, or done anything with the software
not covered by your licenses, your licenses can nonetheless
continue if you come into full compliance with these terms,
and take practical steps to correct past violations, within
32 days of receiving notice.  Otherwise, all your licenses
end immediately.

## No Liability

***As far as the law allows, the software comes as is, without
any warranty or condition, and the licensor will not be liable
to you for any damages arising out of these terms or the use
or nature of the software, under any kind of legal claim.***

## Definitions

The **licensor** is the individual or entity offering these
terms, and the **software** is the software the licensor makes
available under these terms.

**You** refers to the individual or entity agreeing to these
terms.

**Your company** is any legal entity, sole proprietorship,
or other kind of organization that you work for, plus all
organizations that have control over, are under the control of,
or are under common control with that organization.  **Control**
means ownership of substantially all the assets of an entity,
or the power to direct its management and policies by vote,
contract, or otherwise.  Control can be direct or indirect.

**Your licenses** are all the licenses granted to you for the
software under these terms.

**Use** means anything you do with the software requiring one
of your licenses.
