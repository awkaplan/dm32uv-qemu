# SPDX-License-Identifier: LGPL-2.1-or-later
csky_ss = ss.source_set()
csky_ss.add(files('boot.c'))
csky_ss.add(when: 'CONFIG_SMARTL', if_true: files('smartl.c'))
csky_ss.add(when: 'CONFIG_DM32', if_true: files('dm32.c'))

hw_arch += {'csky': csky_ss}
