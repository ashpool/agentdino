#!/usr/bin/perl
# Usage: debug.pl   (Ctrl-C to quit)
# Live view of the board over USB serial ($AGENTDINO_SERIAL, default the first /dev/cu.usbmodem*): the LED matrix,
# the lane table and the last packets received. The board streams only while it gets a "?" line every few seconds.
use strict;
use warnings;
use Fcntl qw(O_RDWR O_NONBLOCK O_NOCTTY);
use IO::Select;
use POSIX qw(strftime);

use constant {
    LEASE_REFRESH => 2,
    ROWS          => 8,
    COLS          => 12,
    LOG_LINES     => 10,
};

my %MODES = (W => 'working', D => 'done', P => 'waiting', I => 'input needed');

# "F 07F 05D ...": 8 rows of 12 bits, bit 11 is column 0. Returns 8 strings of '#' and '.', or nothing.
sub parse_frame {
    my ($line) = @_;
    my @rows = $line =~ /^F((?: [0-9A-F]{3}){8})$/ or return;
    return map { sprintf('%012b', hex $_) =~ tr/01/.#/r } split ' ', $rows[0];
}

# "L - 3520:W:2:12 - ...": one entry per lane, '-' when free. Returns hashes (undef for free lanes), or nothing.
sub parse_lanes {
    my ($line) = @_;
    $line =~ /^L((?: \S+)+)$/ or return;
    return map {
        /^(.{4}):([WDPI]):(\d+):(\d+)$/ ? { key => $1, mode => $MODES{$2}, agents => $3, age => $4 } : undef
    } split ' ', $1;
}

sub render {
    my ($state) = @_;
    my @out = ('agentdino debug (Ctrl-C to quit)', '');
    my @frame = @{ $state->{frame} // [ ('.' x COLS) x ROWS ] };
    push @out, '+' . ('-' x (COLS * 2)) . '+';
    push @out, '|' . join('', map { $_ eq '#' ? '██' : '  ' } split //) . '|' for @frame;
    push @out, '+' . ('-' x (COLS * 2)) . '+', '';
    my @lanes = @{ $state->{lanes} // [] };
    push @out, 'lanes:';
    push @out, map {
        my $lane = $lanes[$_];
        $lane
            ? sprintf('  %d  %s  %-12s agents=%d  last command %ds ago', $_, @{$lane}{qw(key mode agents age)})
            : "  $_  -"
    } 0 .. $#lanes;
    push @out, '', 'packets:', map { "  $_" } @{ $state->{log} // [] };
    return join("\n", @out) . "\n";
}

# Folds one line from the board into the state; unknown lines (like the boot IP) are ignored.
sub update {
    my ($state, $line, $time) = @_;
    $line =~ s/\r$//;
    if (my @frame = parse_frame($line)) {
        $state->{frame} = \@frame;
    } elsif (my @lanes = parse_lanes($line)) {
        $state->{lanes} = \@lanes;
    } elsif ($line =~ /^rx (.+)$/) {
        my @log = (@{ $state->{log} // [] }, "$time  $1");
        shift @log while @log > LOG_LINES;
        $state->{log} = \@log;
    } else {
        return 0;
    }
    return 1;
}

sub open_port {
    my ($env) = @_;
    my $path = $env->{AGENTDINO_SERIAL} || (glob '/dev/cu.usbmodem*')[0] or die "no board found on /dev/cu.usbmodem*\n";
    system('stty', '-f', $path, 'raw', '-echo') == 0 or die "stty failed on $path\n";
    sysopen(my $port, $path, O_RDWR | O_NONBLOCK | O_NOCTTY) or die "cannot open $path: $!\n";
    return $port;
}

unless (caller) {
    my $port   = open_port(\%ENV);
    my $select = IO::Select->new($port);
    my ($state, $buffer, $asked) = ({}, '', 0);
    $| = 1;
    print "\e[?25l";    # hide the cursor
    $SIG{INT} = sub { print "\e[?25h\n"; exit 0 };
    while (1) {
        if (time - $asked >= LEASE_REFRESH) {
            syswrite($port, "?\n");
            $asked = time;
        }
        next unless $select->can_read(0.5);
        sysread($port, my $chunk, 4096) or next;
        $buffer .= $chunk;
        my $changed = 0;
        $changed |= update($state, $1, strftime('%H:%M:%S', localtime)) while $buffer =~ s/^([^\n]*)\n//;
        print "\e[H\e[J", render($state) if $changed;
    }
}

1;
