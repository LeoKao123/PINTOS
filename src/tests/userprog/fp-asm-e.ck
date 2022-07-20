# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(fp-asm-e) begin
(fp-asm-e) Computing e...
(fp-asm) begin
(fp-asm) Starting...
fp-asm-helper: exit(0)
fp-asm: exit(162)
(fp-asm-e) Success!
fp-asm-e: exit(162)
EOF
pass;
