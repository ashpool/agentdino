# Tests the parsing and rendering in debug.pl; no board involved.
use strict;
use warnings;
use FindBin qw($Bin);
use File::Temp qw(tempdir);
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

# parse_frame / parse_lanes edge cases
is(scalar(() = main::parse_frame("F 07F 05D 07F 06B 577 7F8 FF8 330\r")), 8, 'a frame with a trailing carriage return is accepted');
is(scalar(() = main::parse_frame('F 07F 05D 07F 06B 577 7F8 FF8')), 0, 'a frame with 7 rows is rejected');
is(scalar(() = main::parse_frame('F 07f 05d 07f 06b 577 7f8 ff8 330')), 0, 'lowercase hex is rejected');
is(scalar(() = main::parse_lanes('L')), 0, 'a bare L line is an empty lane table');
is_deeply([ map { defined $_ ? 1 : 0 } main::parse_lanes('L a:W:0:0 b:D:0:0') ], [ 0, 0 ], 'keys must be 4 characters');
is_deeply(
    [ map { $_->{mode} } main::parse_lanes('L aaaa:W:0:0 bbbb:P:0:0 cccc:I:0:0') ],
    [ 'working', 'idle', 'input needed' ],
    'every mode code is named'
);
is_deeply([ map { defined $_ ? 1 : 0 } main::parse_lanes('L aaaa:W:0:0 garbage - ab:cd:W:1:1') ], [ 1, 0, 0, 0 ], 'a malformed entry becomes a free lane in place');
is(((main::parse_lanes('L a:b::W:1:2'))[0] // {})->{key}, 'a:b:', 'a colon inside the key is part of the key');

# printable
is(main::printable("ab\e[2J\n\x07x"), 'ab?[2J??x', 'control characters become question marks');
unlike(main::render({ log => ["10:00:00  abcd:\e[2J"], lanes => [ { key => "\e[2J", mode => 'working', agents => 0, age => 0 } ] }), qr/\e/, 'render never emits control characters from the board');

# feed
my ($fstate, $buffer) = ({}, '');
ok(!main::feed($fstate, \$buffer, 'F 07F 0', '10:00:00'), 'half a line changes nothing');
ok(main::feed($fstate, \$buffer, "5D 07F 06B 577 7F8 FF8 330\r\nrx abcd:W\r\nL - -\r\n", '10:00:01'), 'the rest of the line and two more lines in one chunk');
is($fstate->{frame}[1], '.....#.###.#', 'the split frame line was reassembled');
is_deeply($fstate->{log}, ['10:00:01  abcd:W'], 'the packet line in the same chunk was logged without CR');
is(scalar @{ $fstate->{lanes} }, 2, 'the lanes line was parsed');
is($buffer, '', 'nothing is left in the buffer');
main::feed($fstate, \$buffer, 'x' x 10_000, '10:00:02');
cmp_ok(length $buffer, '<=', 4096, 'a stream without newlines does not grow the buffer');

# serial_paths / open_port
is_deeply([ main::serial_paths({ AGENTDINO_SERIAL => '/dev/x' }) ], ['/dev/x'], 'AGENTDINO_SERIAL is the only candidate when set');
my $dir = tempdir(CLEANUP => 1);
like(eval { main::open_port({ AGENTDINO_SERIAL => "$dir/missing" }); 1 } ? '' : $@, qr/cannot open .*missing/, 'a missing port dies with its path');
open(my $file, '>', "$dir/plain") or die;
close $file;
like(eval { main::open_port({ AGENTDINO_SERIAL => "$dir/plain" }); 1 } ? '' : $@, qr/is not a serial port/, 'a regular file is rejected');

# render status
like(main::render({ port => '/dev/cu.usbmodem1', status => '- no data for 5s' }), qr{^agentdino debug /dev/cu.usbmodem1 - no data for 5s}, 'the header shows the port and status');

done_testing;
