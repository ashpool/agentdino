# Tests signal.pl against local UDP listeners; no board or external network involved.
use strict;
use warnings;
use File::Temp qw(tempdir);
use FindBin qw($Bin);
use IO::Select;
use IO::Socket::INET;
use POSIX qw(mkfifo);
use Test::More;
use Time::HiRes qw(time);

require "$Bin/../signal.pl";

sub listener {
    my ($addr) = @_;
    my $listener = IO::Socket::INET->new(Proto => 'udp', LocalAddr => $addr, LocalPort => 0) or die "listen: $!";
    return $listener;
}

sub received {
    my ($listener) = @_;
    return '' unless IO::Select->new($listener)->can_read(2);
    $listener->recv(my $data, 64);
    return $data;
}

# Runs the real script with $input piped to its stdin (or /dev/null for undef) and returns its exit code.
sub run_signal {
    my ($listener, $commands, $input) = @_;
    local $ENV{AGENTDINO_HOST} = '127.0.0.1';
    local $ENV{AGENTDINO_PORT} = $listener->sockport;
    open(my $pipe, '|-', $^X, "$Bin/../signal.pl", $commands) or die "spawn: $!";
    print {$pipe} $input if defined $input;
    close $pipe;
    return $? >> 8;
}

my $hook_json = '{"session_id":"abcd1234-0000","hook_event_name":"Stop"}';

# session_key
is(main::session_key($hook_json), 'abcd', 'session key is the first 4 chars of session_id');
is(main::session_key('{"hook_event_name":"Stop"}'), '0000', 'falls back when there is no session_id');
is(main::session_key(''), '0000', 'falls back on empty input');
is(main::session_key('{"session_id":"ab"}'), '0000', 'falls back when session_id is shorter than 4 chars');
is(main::session_key('{"session_id":"a:b1-xyz"}'), '0000', 'rejects keys that would break the "<key>:" framing');
is(
    main::session_key('{"tool_input":"{\"session_id\":\"zzzz\"}","session_id":"abcd-1"}'),
    'abcd',
    'ignores an escaped session_id nested inside tool input'
);

# agent_key / with_agent
is(main::agent_key('{"session_id":"abcd-1","agent_id":"a84cd8914fcfd5eb7"}'), '5eb7', 'agent key is the last 4 chars of agent_id');
is(main::agent_key('{"session_id":"abcd-1"}'), '0000', 'agent key falls back when there is no agent_id');
is(main::with_agent('S', '{"agent_id":"a84cd8914fcfd5eb7"}'), 'S5eb7', 'S carries the agent key');
is(main::with_agent('s', '{"agent_id":"a84cd8914fcfd5eb7"}'), 's5eb7', 's carries the agent key');
is(main::with_agent('S', ''), 'S0000', 'S without agent_id uses the fallback key');
is(main::with_agent('OW', '{"agent_id":"a84cd8914fcfd5eb7"}'), 'OW', 'other commands are left alone');

# serial_path / send_serial
is(main::serial_path({ AGENTDINO_HOST => '127.0.0.1' }), undef, 'AGENTDINO_HOST selects UDP over the serial glob');
is(main::serial_path({ AGENTDINO_HOST => '127.0.0.1', AGENTDINO_SERIAL => '/dev/x' }), '/dev/x', 'AGENTDINO_SERIAL wins over AGENTDINO_HOST');
is(main::serial_path({ AGENTDINO_SERIAL => '/dev/x' }), '/dev/x', 'AGENTDINO_SERIAL overrides the device path');
{
    my $dir = tempdir(CLEANUP => 1);
    my $file = "$dir/port";
    open(my $touch, '>', $file) or die "create: $!";
    close $touch;
    ok(main::send_commands('OW', $hook_json, { AGENTDINO_SERIAL => $file }), 'serial send succeeds');
    open(my $in, '<', $file) or die "read: $!";
    is(do { local $/; <$in> }, "abcd:OW\n", 'serial gets "<key>:<commands>" as a line');

    my $fallback = listener('127.0.0.1');
    my $env = { AGENTDINO_SERIAL => "$dir/missing", AGENTDINO_HOST => '127.0.0.1', AGENTDINO_PORT => $fallback->sockport };
    ok(main::send_commands('W', $hook_json, $env), 'a missing serial port falls back to UDP');
    is(received($fallback), 'abcd:W', 'the fallback datagram carries the message');

    mkfifo("$dir/fifo", 0600) or die "mkfifo: $!";
    ok(!main::send_serial("$dir/fifo", 'abcd:W'), 'a port nobody reads fails immediately instead of blocking');
}

# send_commands / open_socket
my $unicast = listener('127.0.0.1');
ok(main::send_commands('OW', $hook_json, { AGENTDINO_HOST => '127.0.0.1', AGENTDINO_PORT => $unicast->sockport }), 'unicast send succeeds');
is(received($unicast), 'abcd:OW', 'unicast datagram is "<key>:<commands>"');

# macOS does not loop broadcasts back to local sockets, so check the target instead of delivery.
my $default = main::open_socket({ AGENTDINO_PORT => 4999 });
is($default->peerhost, '255.255.255.255', 'targets the LAN broadcast address when AGENTDINO_HOST is unset');
is($default->peerport, 4999, 'honours AGENTDINO_PORT');
ok($default->sockopt(SO_BROADCAST), 'broadcast is enabled on the socket');

ok(!main::send_commands('W', '', { AGENTDINO_HOST => '256.256.256.256' }), 'unresolvable host does not die');
ok(!main::send_commands('W', undef, { AGENTDINO_HOST => '127.0.0.1' }), 'sends nothing when there was no stdin answer');

# end to end through the real script
my $e2e = listener('127.0.0.1');
is(run_signal($e2e, 'OW', $hook_json), 0, 'script exits 0 with hook JSON on stdin');
is(received($e2e), 'abcd:OW', 'session key is read from stdin end to end');

my $subagent_json = '{"session_id":"abcd1234-0000","agent_id":"a84cd8914fcfd5eb7","hook_event_name":"SubagentStop"}';
is(run_signal($e2e, 's', $subagent_json), 0, 'script exits 0 for a subagent event');
is(received($e2e), 'abcd:s5eb7', 'subagent stop carries session key and agent key end to end');

is(run_signal($e2e, 'W', undef), 0, 'script exits 0 with empty stdin');
is(received($e2e), '0000:W', 'empty stdin is a manual call and uses the 0000 key');

my $big = '{"session_id":"wxyz-1","tool_response":"' . ('x' x 300_000) . '"}';
is(run_signal($e2e, 'W', $big), 0, 'script exits 0 and does not break the writer on a large payload');
is(received($e2e), 'wxyz:W', 'session key is found at the front of a large payload');

{
    local $ENV{AGENTDINO_HOST} = '127.0.0.1';
    local $ENV{AGENTDINO_PORT} = $e2e->sockport;
    my $start = time;
    open(my $silent, '|-', $^X, "$Bin/../signal.pl", 'W') or die "spawn: $!";
    is(received($e2e), '', 'sends nothing when stdin stays open and silent');
    close $silent;
    cmp_ok(time - $start, '<', 6, 'does not hang on an open but silent stdin');
}

{
    local $ENV{AGENTDINO_HOST} = '256.256.256.256';
    open(my $pipe, '|-', $^X, "$Bin/../signal.pl", 'W') or die "spawn: $!";
    print {$pipe} $hook_json;
    close $pipe;
    is($? >> 8, 0, 'script exits 0 when the host cannot be resolved');
}

done_testing;
