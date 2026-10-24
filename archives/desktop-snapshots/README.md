# Desktop historical source snapshots

These are historical versions, not replacements for the current source tree. The main branch remains the current validated application.

Each ZIP preserves original internal paths and contains `SOURCE_ARCHIVE_MANIFEST.json` with per-file SHA256 checksums. Credentials, ESP-NOW keys, compiled firmware (which embeds keys), executables and caches are excluded. Complete original ZIPs remain in the local project backup directory.

| Original desktop backup | Sanitized source snapshot | Files |
|---|---|---|
| PID_P启停_I模式_修改前备份_20261023.zip | [PID_P启停_I模式_修改前备份_20261023-source-only.zip](PID_P%E5%90%AF%E5%81%9C_I%E6%A8%A1%E5%BC%8F_%E4%BF%AE%E6%94%B9%E5%89%8D%E5%A4%87%E4%BB%BD_20261023-source-only.zip) | 1600 |
| PID_P按键S弯版本_恢复前备份_20261024.zip | [PID_P按键S弯版本_恢复前备份_20261024-source-only.zip](PID_P%E6%8C%89%E9%94%AES%E5%BC%AF%E7%89%88%E6%9C%AC_%E6%81%A2%E5%A4%8D%E5%89%8D%E5%A4%87%E4%BB%BD_20261024-source-only.zip) | 1575 |
| PID_无线串口版_20261024.zip | [PID_无线串口版_20261024-source-only.zip](PID_%E6%97%A0%E7%BA%BF%E4%B8%B2%E5%8F%A3%E7%89%88_20261024-source-only.zip) | 2239 |
| PID_无线烧录已验证_20261023_1838.zip | [PID_无线烧录已验证_20261023_1838-source-only.zip](PID_%E6%97%A0%E7%BA%BF%E7%83%A7%E5%BD%95%E5%B7%B2%E9%AA%8C%E8%AF%81_20261023_1838-source-only.zip) | 1600 |
| PID_无线烧录提速版_20261024.zip | [PID_无线烧录提速版_20261024-source-only.zip](PID_%E6%97%A0%E7%BA%BF%E7%83%A7%E5%BD%95%E6%8F%90%E9%80%9F%E7%89%88_20261024-source-only.zip) | 1633 |
| PID_项目修复_实板已验证_20261024.zip | [PID_项目修复_实板已验证_20261024-source-only.zip](PID_%E9%A1%B9%E7%9B%AE%E4%BF%AE%E5%A4%8D_%E5%AE%9E%E6%9D%BF%E5%B7%B2%E9%AA%8C%E8%AF%81_20261024-source-only.zip) | 2301 |
| PID_项目检查修复版_20261024.zip | [PID_项目检查修复版_20261024-source-only.zip](PID_%E9%A1%B9%E7%9B%AE%E6%A3%80%E6%9F%A5%E4%BF%AE%E5%A4%8D%E7%89%88_20261024-source-only.zip) | 2270 |

The earliest snapshots also contain the historical `STM32_f407` vehicle project. Newer snapshots contain validation project sources under their original locations. Use the current root README for normal builds.
