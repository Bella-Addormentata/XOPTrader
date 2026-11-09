# Third-party notices

XOPTrader is licensed under the WTFPL, Version 2. See [LICENSE](LICENSE).

This file lists work by other people that XOPTrader includes or ships with.

## Code and data in this repository

The files below contain work by other people. That work is **not** covered by
the WTFPL. It keeps its original license, and its notice below has to travel
with it when you copy or redistribute it. All of these licenses are
permissive. The rest of each listed file is XOPTrader's own work under the
WTFPL.

| Path | What it is | License |
|---|---|---|
| `gui/services/warp/puzzles/portal_receiver.clsp.hex`, `message_coin.clsp.hex`, `bridging_puzzle.clsp.hex`, `wrapped_tail.clsp.hex`, `cat_minter.clsp.hex`, `cat_mint_and_payout.clsp.hex` | Compiled CLVM puzzles from [warpdotgreen/cli](https://github.com/warpdotgreen/cli), unmodified | MIT, [warp.green](#warpgreen-mit) |
| `gui/services/warp/puzzles/cat_burner.clsp.hex`, `burn_inner_puzzle.clsp.hex` | Compiled CLVM puzzles from [warpdotgreen/warp-ui](https://github.com/warpdotgreen/warp-ui) `drivers/erc20bridge.tsx`, unmodified | MIT, [warp.green](#warpgreen-mit) |
| `gui/services/warp/puzzles/singleton_top_layer_v1_1.clsp.hex`, `cat_v2.clsp.hex`, `p2_delegated_puzzle_or_hidden_puzzle.clsp.hex`, `p2_m_of_n_delegate_direct.clsp.hex` | Compiled CLVM puzzles from [Chia-Network/chia-blockchain](https://github.com/Chia-Network/chia-blockchain), unmodified | Apache-2.0, [Chia Network](#chia-network-apache-20) |
| `gui/services/warp/drivers.py` | Ported from warpdotgreen/cli `drivers/portal.py` and `drivers/wrapped_assets.py`, warp-ui `drivers/erc20bridge.tsx` (`mintCATs`), and Chia's `singleton_top_layer_v1_1` driver | MIT, [warp.green](#warpgreen-mit); Apache-2.0, [Chia Network](#chia-network-apache-20) |
| `gui/services/warp/constants.py` | Deployment values transcribed from warp-ui `config.tsx` | MIT, [warp.green](#warpgreen-mit) |
| `gui/services/warp/tests/test_warp_clvm_anchors.py` | Mint derivation ported from warpdotgreen/cli `drivers/wrapped_assets.py` | MIT, [warp.green](#warpgreen-mit) |
| `gui/services/warp/tests/test_warp_validator_policy.py` | `validate_chia_erc20_unwrap` ported from warpdotgreen/cli `commands/spend_policy.py` | MIT, [warp.green](#warpgreen-mit) |
| `gui/services/warp/clvm_utils.py`, the bech32m section | Pieter Wuille's bech32 reference implementation, as carried in Chia's `chia/util/bech32m.py` | MIT, [Pieter Wuille](#bech32m-mit) |
| `gui/services/warp/tests/fixtures_bip340_vectors.csv` | [BIP 340](https://github.com/bitcoin/bips/blob/master/bip-0340.mediawiki) test vectors (`bip-0340/test-vectors.csv`), content unchanged | CC0-1.0, [BIP 340](#bip-340-test-vectors-cc0-10) |

### warp.green (MIT)

```text
MIT License

Copyright (c) 2024 Mihai Dancaescu (yakuhito)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### Chia Network (Apache-2.0)

```text
                                 Apache License
                           Version 2.0, January 2004
                        http://www.apache.org/licenses/

   TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION

   1. Definitions.

      "License" shall mean the terms and conditions for use, reproduction,
      and distribution as defined by Sections 1 through 9 of this document.

      "Licensor" shall mean the copyright owner or entity authorized by
      the copyright owner that is granting the License.

      "Legal Entity" shall mean the union of the acting entity and all
      other entities that control, are controlled by, or are under common
      control with that entity. For the purposes of this definition,
      "control" means (i) the power, direct or indirect, to cause the
      direction or management of such entity, whether by contract or
      otherwise, or (ii) ownership of fifty percent (50%) or more of the
      outstanding shares, or (iii) beneficial ownership of such entity.

      "You" (or "Your") shall mean an individual or Legal Entity
      exercising permissions granted by this License.

      "Source" form shall mean the preferred form for making modifications,
      including but not limited to software source code, documentation
      source, and configuration files.

      "Object" form shall mean any form resulting from mechanical
      transformation or translation of a Source form, including but
      not limited to compiled object code, generated documentation,
      and conversions to other media types.

      "Work" shall mean the work of authorship, whether in Source or
      Object form, made available under the License, as indicated by a
      copyright notice that is included in or attached to the work
      (an example is provided in the Appendix below).

      "Derivative Works" shall mean any work, whether in Source or Object
      form, that is based on (or derived from) the Work and for which the
      editorial revisions, annotations, elaborations, or other modifications
      represent, as a whole, an original work of authorship. For the purposes
      of this License, Derivative Works shall not include works that remain
      separable from, or merely link (or bind by name) to the interfaces of,
      the Work and Derivative Works thereof.

      "Contribution" shall mean any work of authorship, including
      the original version of the Work and any modifications or additions
      to that Work or Derivative Works thereof, that is intentionally
      submitted to Licensor for inclusion in the Work by the copyright owner
      or by an individual or Legal Entity authorized to submit on behalf of
      the copyright owner. For the purposes of this definition, "submitted"
      means any form of electronic, verbal, or written communication sent
      to the Licensor or its representatives, including but not limited to
      communication on electronic mailing lists, source code control systems,
      and issue tracking systems that are managed by, or on behalf of, the
      Licensor for the purpose of discussing and improving the Work, but
      excluding communication that is conspicuously marked or otherwise
      designated in writing by the copyright owner as "Not a Contribution."

      "Contributor" shall mean Licensor and any individual or Legal Entity
      on behalf of whom a Contribution has been received by Licensor and
      subsequently incorporated within the Work.

   2. Grant of Copyright License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      copyright license to reproduce, prepare Derivative Works of,
      publicly display, publicly perform, sublicense, and distribute the
      Work and such Derivative Works in Source or Object form.

   3. Grant of Patent License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      (except as stated in this section) patent license to make, have made,
      use, offer to sell, sell, import, and otherwise transfer the Work,
      where such license applies only to those patent claims licensable
      by such Contributor that are necessarily infringed by their
      Contribution(s) alone or by combination of their Contribution(s)
      with the Work to which such Contribution(s) was submitted. If You
      institute patent litigation against any entity (including a
      cross-claim or counterclaim in a lawsuit) alleging that the Work
      or a Contribution incorporated within the Work constitutes direct
      or contributory patent infringement, then any patent licenses
      granted to You under this License for that Work shall terminate
      as of the date such litigation is filed.

   4. Redistribution. You may reproduce and distribute copies of the
      Work or Derivative Works thereof in any medium, with or without
      modifications, and in Source or Object form, provided that You
      meet the following conditions:

      (a) You must give any other recipients of the Work or
          Derivative Works a copy of this License; and

      (b) You must cause any modified files to carry prominent notices
          stating that You changed the files; and

      (c) You must retain, in the Source form of any Derivative Works
          that You distribute, all copyright, patent, trademark, and
          attribution notices from the Source form of the Work,
          excluding those notices that do not pertain to any part of
          the Derivative Works; and

      (d) If the Work includes a "NOTICE" text file as part of its
          distribution, then any Derivative Works that You distribute must
          include a readable copy of the attribution notices contained
          within such NOTICE file, excluding those notices that do not
          pertain to any part of the Derivative Works, in at least one
          of the following places: within a NOTICE text file distributed
          as part of the Derivative Works; within the Source form or
          documentation, if provided along with the Derivative Works; or,
          within a display generated by the Derivative Works, if and
          wherever such third-party notices normally appear. The contents
          of the NOTICE file are for informational purposes only and
          do not modify the License. You may add Your own attribution
          notices within Derivative Works that You distribute, alongside
          or as an addendum to the NOTICE text from the Work, provided
          that such additional attribution notices cannot be construed
          as modifying the License.

      You may add Your own copyright statement to Your modifications and
      may provide additional or different license terms and conditions
      for use, reproduction, or distribution of Your modifications, or
      for any such Derivative Works as a whole, provided Your use,
      reproduction, and distribution of the Work otherwise complies with
      the conditions stated in this License.

   5. Submission of Contributions. Unless You explicitly state otherwise,
      any Contribution intentionally submitted for inclusion in the Work
      by You to the Licensor shall be under the terms and conditions of
      this License, without any additional terms or conditions.
      Notwithstanding the above, nothing herein shall supersede or modify
      the terms of any separate license agreement you may have executed
      with Licensor regarding such Contributions.

   6. Trademarks. This License does not grant permission to use the trade
      names, trademarks, service marks, or product names of the Licensor,
      except as required for reasonable and customary use in describing the
      origin of the Work and reproducing the content of the NOTICE file.

   7. Disclaimer of Warranty. Unless required by applicable law or
      agreed to in writing, Licensor provides the Work (and each
      Contributor provides its Contributions) on an "AS IS" BASIS,
      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
      implied, including, without limitation, any warranties or conditions
      of TITLE, NON-INFRINGEMENT, MERCHANTABILITY, or FITNESS FOR A
      PARTICULAR PURPOSE. You are solely responsible for determining the
      appropriateness of using or redistributing the Work and assume any
      risks associated with Your exercise of permissions under this License.

   8. Limitation of Liability. In no event and under no legal theory,
      whether in tort (including negligence), contract, or otherwise,
      unless required by applicable law (such as deliberate and grossly
      negligent acts) or agreed to in writing, shall any Contributor be
      liable to You for damages, including any direct, indirect, special,
      incidental, or consequential damages of any character arising as a
      result of this License or out of the use or inability to use the
      Work (including but not limited to damages for loss of goodwill,
      work stoppage, computer failure or malfunction, or any and all
      other commercial damages or losses), even if such Contributor
      has been advised of the possibility of such damages.

   9. Accepting Warranty or Additional Liability. While redistributing
      the Work or Derivative Works thereof, You may choose to offer,
      and charge a fee for, acceptance of support, warranty, indemnity,
      or other liability obligations and/or rights consistent with this
      License. However, in accepting such obligations, You may act only
      on Your own behalf and on Your sole responsibility, not on behalf
      of any other Contributor, and only if You agree to indemnify,
      defend, and hold each Contributor harmless for any liability
      incurred by, or claims asserted against, such Contributor by reason
      of your accepting any such warranty or additional liability.

   END OF TERMS AND CONDITIONS

   APPENDIX: How to apply the Apache License to your work.

      To apply the Apache License to your work, attach the following
      boilerplate notice, with the fields enclosed by brackets "[]"
      replaced with your own identifying information. (Don't include
      the brackets!)  The text should be enclosed in the appropriate
      comment syntax for the file format. We also recommend that a
      file or class name and description of purpose be included on the
      same "printed page" as the copyright notice for easier
      identification within third-party archives.

   Copyright 2026 Chia Network Inc.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
```

### bech32m (MIT)

```text
Copyright (c) 2017 Pieter Wuille

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

### BIP 340 test vectors (CC0-1.0)

BIP 340 offers its test vectors and reference code under BSD-2-Clause, MIT or
CC0 1.0, at the user's choice. XOPTrader takes them under
[CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/), which asks for
no notice. They are listed here for credit: BIP 340 is by Pieter Wuille,
Jonas Nick and Tim Ruffing.

## Release builds

The release builds (the Windows installer, the GUI and engine binaries, and the
release zip) also contain third-party software that is not part of this
repository. Each piece keeps its own license:

- **Python** (PSF-2.0) and the Python packages pinned in
  `gui/requirements.lock.txt`. Almost all of these are MIT, BSD, Apache-2.0 or
  PSF-2.0. certifi is MPL-2.0.
- **Qt for Python** (PySide6, shiboken6) and the Qt libraries it carries. They
  are offered under LGPL-3.0, GPL-2.0 or GPL-3.0, and XOPTrader uses them under
  LGPL-3.0. Source: <https://code.qt.io/cgit/pyside/pyside-setup.git/> and
  <https://download.qt.io/official_releases/qt/>. You can build the GUI against
  your own build of Qt from this repository's source; see
  `.github/workflows/release.yml`.
- **The C++ engine's libraries**, from `cpp/vcpkg.json`: Boost (BSL-1.0),
  OpenSSL (Apache-2.0), curl (curl license), nlohmann-json, spdlog, yaml-cpp
  and prometheus-cpp (MIT), SQLite (public domain), and what they depend on.
- **The PyInstaller bootloader** (GPL-2.0 with an exception that lets it be
  bundled with programs under any license).
- **The Microsoft Visual C++ Redistributable** on Windows, under Microsoft's
  own terms.
