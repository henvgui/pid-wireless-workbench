"""Create this pair's local ESP-NOW keys once. Both builds use the same file."""
from pathlib import Path
import secrets

target = Path(__file__).parent/'private'/'pair_keys.h'
if target.exists():
    print('Existing pair keys retained')
else:
    target.parent.mkdir(parents=True, exist_ok=True)
    lines = ['#pragma once', '#include <stdint.h>', 'namespace pidlink {']
    for name in ('kPrimaryKey', 'kPairKey'):
        values = ','.join('0x%02x' % b for b in secrets.token_bytes(16))
        lines.append('static const uint8_t %s[16]={%s};' % (name, values))
    lines.append('}')
    target.write_text('\n'.join(lines)+'\n', encoding='ascii')
    print('Created local pair keys; values are not printed')
