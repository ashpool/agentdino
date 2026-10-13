# Tests the parsing and rendering in debug.pl; no board involved.
use strict;
use warnings;
use FindBin qw($Bin);
use Test::More;

require "$Bin/../debug.pl";

# parse_frame
my @frame = main::parse_frame('F 07F 05D 07F 06B 577 7F8 FF8 330');
is(scalar @frame, 8, 'a frame has 8 rows');
is($frame[0], '.....#######', 'bit 11 is column 0');
is($frame[7], '..##..##....', 'the last row is decoded');
is(scalar(() = main::parse_frame('F 07F 05D')), 0, 'a short frame is rejected');
is(scalar(() = main::parse_frame('rx abcd:W')), 0, 'other lines are not frames');

# parse_lanes
my @lanes = main::parse_lanes('L - 3520:W:2:12 - - ab12:I:0:4 -');
is(scalar @lanes, 6, 'one entry per lane');
is($lanes[0], undef, 'a free lane is undef');
is_deeply($lanes[1], { key => '3520', mode => 'working', agents => 2, age => 12 }, 'an active lane is decoded');
is($lanes[4]{mode}, 'input needed', 'mode codes are named');
is($lanes[2], undef, 'free lanes between active ones stay in place');

# update
my $state = {};
ok(main::update($state, "rx 3520:W\r", '10:00:00'), 'a packet line changes the state');
ok(main::update($state, 'F 07F 05D 07F 06B 577 7F8 FF8 330', '10:00:01'), 'a frame line changes the state');
ok(main::update($state, 'L - - - - - -', '10:00:02'), 'a lanes line changes the state');
ok(!main::update($state, '192.168.1.20', '10:00:03'), 'unknown lines such as the boot IP are ignored');
is_deeply($state->{log}, ['10:00:00  3520:W'], 'the packet is logged with its time and without the line ending');
main::update($state, "rx x$_:W", '10:00:04') for 1 .. 12;
is(scalar @{ $state->{log} }, 10, 'the packet log keeps the last 10 lines');
is($state->{log}[-1], '10:00:04  x12:W', 'the newest packet is last');

# render
my $screen = main::render({});
like($screen, qr/^agentdino debug/, 'renders a title with no data yet');
$state->{lanes} = [ undef, { key => '3520', mode => 'working', agents => 1, age => 3 } ];
$screen = main::render($state);
like($screen, qr/1  3520  working\s+agents=1  last command 3s ago/, 'renders an active lane');
like($screen, qr/^  0  -$/m, 'renders a free lane');
like($screen, qr/^\|.{0}(?:  ){5}(?:██){7}\|$/m, 'renders lit pixels as double-width blocks');
like($screen, qr/10:00:04  x12:W/, 'renders the packet log');

done_testing;
