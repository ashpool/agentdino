#!/usr/bin/perl
# Usage: signal.pl <commands>, e.g. W, OW, D, I, S, s, O.
# Sends "<4-char session key>:<commands>"; S and s are followed by a 4-char agent id. Without $AGENTDINO_HOST it
# is written as a line to the board's USB serial port ($AGENTDINO_SERIAL, default the first /dev/cu.usbmodem*).
# Otherwise, or if that write fails, it is one UDP datagram to $AGENTDINO_HOST (an IP; a LAN broadcast when unset) on port
# $AGENTDINO_PORT (default 4210). The key comes from the hook JSON on stdin; with no stdin data
# (manual use) it is 0000, and if stdin is open but silent nothing is sent. Exits 0 even when sending fails.
use strict;
use warnings;
use Fcntl qw(O_WRONLY O_NONBLOCK O_NOCTTY);
use IO::Select;
use IO::Socket::INET;
use Time::HiRes qw(alarm);

use constant {
    BROADCAST    => '255.255.255.255',
    DEFAULT_PORT => 4210,
    NO_ID        => '0000',
    STDIN_WAIT   => 1,
    HOOK_BUDGET  => 1.5,
};

sub session_key {
    my ($json) = @_;
    return $json =~ /"session_id"\s*:\s*"([0-9A-Za-z_-]{4})/ ? $1 : NO_ID;
}

# The last 4 chars of agent_id: the leading chars are not random enough to tell agents apart.
sub agent_key {
    my ($json) = @_;
    return $json =~ /"agent_id"\s*:\s*"[0-9A-Za-z_-]*([0-9A-Za-z_-]{4})"/ ? $1 : NO_ID;
}

sub with_agent {
    my ($commands, $json) = @_;
    return $commands =~ /^[Ss]$/ ? $commands . agent_key($json) : $commands;
}

# The first chunk is enough for session_id. Returns '' for a terminal or empty stdin, and nothing if stdin is
# open but stays silent.
sub read_hook_input {
    return '' if -t STDIN;
    return unless IO::Select->new(\*STDIN)->can_read(STDIN_WAIT);
    my $read = sysread(STDIN, my $chunk, 4096);
    return $read ? $chunk : '';
}

# Hook payloads can be large; reading them out keeps the writer from getting EPIPE when we exit.
sub drain_hook_input {
    my $select = IO::Select->new(\*STDIN);
    1 while !-t STDIN && $select->can_read(0.1) && sysread(STDIN, my $discard, 65536);
}

sub open_socket {
    my ($env) = @_;
    return IO::Socket::INET->new(
        Proto     => 'udp',
        PeerAddr  => $env->{AGENTDINO_HOST} || BROADCAST,
        PeerPort  => $env->{AGENTDINO_PORT} || DEFAULT_PORT,
        Broadcast => 1,
    );
}

# AGENTDINO_SERIAL wins; otherwise an explicit AGENTDINO_HOST means UDP; otherwise the USB serial port, if the board is plugged in.
sub serial_path {
    my ($env) = @_;
    return $env->{AGENTDINO_SERIAL} if $env->{AGENTDINO_SERIAL};
    return if $env->{AGENTDINO_HOST};
    return (glob '/dev/cu.usbmodem*')[0];
}

# Non-blocking so a stuck port cannot hang the hook; a short write would garble the next line, so it counts as failure.
sub send_serial {
    my ($path, $message) = @_;
    sysopen(my $port, $path, O_WRONLY | O_NONBLOCK | O_NOCTTY) or return 0;
    my $line = "$message\n";
    return (syswrite($port, $line) // -1) == length $line ? 1 : 0;
}

sub send_udp {
    my ($message, $env) = @_;
    my $socket = open_socket($env) or return 0;
    return defined $socket->send($message) ? 1 : 0;
}

sub send_commands {
    my ($commands, $input, $env) = @_;
    return 0 unless defined $input;
    my $message = session_key($input) . ':' . with_agent($commands, $input);
    my $path = serial_path($env);
    return ($path && send_serial($path, $message)) || send_udp($message, $env);
}

unless (caller) {
    my $commands = shift @ARGV // die "usage: signal.pl <commands>\n";
    $SIG{ALRM} = sub { exit 0 };  # a stuck tty must not outlast the 2 s hook timeout
    alarm HOOK_BUDGET;
    my $input = read_hook_input();
    send_commands($commands, $input, \%ENV);
    drain_hook_input();
    exit 0;
}

1;
