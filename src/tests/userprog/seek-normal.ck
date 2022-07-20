# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected (IGNORE_USER_FAULTS => 1, [<<'EOF']);
(seek-normal) begin
seek-normal: exit(-1)
EOF
pass;
