#!/usr/bin/perl
# Usage: debug.pl   (Ctrl-C to quit)
# Live view of the board over USB serial ($AGENTDINO_SERIAL, default the first /dev/cu.usbmodem*): the LED matrix,
# the lane table and the last packets received. The board streams only while it gets a "?" line every few seconds.
# If the board goes away (unplug, reset, upload) it waits and reconnects.
use strict;
use warnings;
use Fcntl qw(O_RDWR O_NONBLOCK O_NOCTTY);
use IO::Select;
use POSIX qw(strftime :termios_h);

use constant {
    LEASE_REFRESH => 2,
    STALE_AFTER   => 4,
    ROWS          => 8,
    COLS          => 12,
    LOG_LINES     => 10,
    MAX_BUFFER    => 4096,
};

my $cursor_hidden = 0;
END { print "\e[?25h" if $cursor_hidden }

my %MODES = (W => 'working', P => 'idle', I => 'input needed');

# "F 07F 05D ...": 8 rows of 12 bits, bit 11 is column 0. Returns 8 strings of '#' and '.', or nothing.
sub parse_frame {
    my ($line) = @_;
    my @rows = $line =~ /^F((?: [0-9A-F]{3}){8})\r?$/ or return;
    return map { sprintf('%012b', hex $_) =~ tr/01/.#/r } split ' ', $rows[0];
}

# "L - 3520:W:2:12 - ...": one entry per lane, '-' when free. Returns hashes (undef for free lanes), or nothing.
sub parse_lanes {
    my ($line) = @_;
    $line =~ /^L((?: \S+)*)\r?$/ or return;
    return map {
        /^(.{4}):([WPI]):(\d+):(\d+)$/ ? { key => $1, mode => $MODES{$2}, agents => $3, age => $4 } : undef
    } split ' ', $1;
}

# Packets come from anyone on the network: never let them reach the terminal as control characters.
sub printable {
    my ($text) = @_;
    return $text =~ s/[^\x20-\x7E]/?/gr;
}

sub render {
    my ($state) = @_;
    my @out = (join(' ', 'agentdino debug', grep { defined } @{$state}{qw(port status)}) . ' (Ctrl-C to quit)', '');
    my @frame = @{ $state->{frame} // [ ('.' x COLS) x ROWS ] };
    push @out, '+' . ('-' x (COLS * 2)) . '+';
    push @out, '|' . join('', map { $_ eq '#' ? '██' : '  ' } split //) . '|' for @frame;
    push @out, '+' . ('-' x (COLS * 2)) . '+', '';
    my @lanes = @{ $state->{lanes} // [] };
    push @out, 'lanes:';
    push @out, map {
        my $lane = $lanes[$_];
        $lane
            ? sprintf('  %d  %s  %-12s agents=%d  last command %ds ago', $_, printable($lane->{key}), @{$lane}{qw(mode agents age)})
            : "  $_  -"
    } 0 .. $#lanes;
    push @out, '', 'packets:', map { '  ' . printable($_) } @{ $state->{log} // [] };
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

# Appends a chunk read from the port and folds every complete line into the state. Returns whether anything changed.
sub feed {
    my ($state, $buffer, $chunk, $time) = @_;
    $$buffer .= $chunk;
    my $changed = 0;
    $changed |= update($state, $1, $time) while $$buffer =~ s/^([^\n]*)\n//;
    substr($$buffer, 0, length($$buffer) - MAX_BUFFER, '') if length $$buffer > MAX_BUFFER;    # no newline in sight: noise
    return $changed;
}

sub serial_paths {
    my ($env) = @_;
    return $env->{AGENTDINO_SERIAL} || glob '/dev/cu.usbmodem*';
}

# Raw 9600 8N1 without hangup on close (that would drop DTR, which the board reads as "host left").
sub set_raw {
    my ($port) = @_;
    my $termios = POSIX::Termios->new;
    $termios->getattr(fileno $port) or return 0;
    $termios->setiflag(0);
    $termios->setoflag(0);
    $termios->setlflag(0);
    $termios->setcflag(($termios->getcflag & ~(PARENB | CSIZE | CSTOPB | HUPCL)) | CS8 | CREAD | CLOCAL);
    $termios->setcc(VMIN, 0);
    $termios->setcc(VTIME, 0);
    $termios->setispeed(B9600);
    $termios->setospeed(B9600);
    return $termios->setattr(fileno $port, TCSANOW) ? 1 : 0;
}

# Dies with a message when there is no usable port.
sub open_port {
    my ($env) = @_;
    my ($path) = serial_paths($env) or die "no board found on /dev/cu.usbmodem*\n";
    sysopen(my $port, $path, O_RDWR | O_NONBLOCK | O_NOCTTY) or die "cannot open $path: $!\n";
    set_raw($port) or die "$path is not a serial port\n";
    return ($port, $path);
}

unless (caller) {
    my $env = \%ENV;
    my ($port, $path, $select, $buffer, $asked, $last_data) = (undef, undef, undef, '', 0, time);
    my $state = {};
    $| = 1;
    print "\e[?25l";    # hide the cursor
    $cursor_hidden = 1;
    $SIG{$_} = sub { exit 0 } for qw(INT TERM HUP);

    # Shows the screen again if anything about it changed.
    my $shown = '';
    my $redraw = sub {
        my $screen = render($state);
        return if $screen eq $shown;
        $shown = $screen;
        print "\e[H\e[J", $screen;
    };

    while (1) {
        unless ($port) {
            my @candidates = serial_paths($env);
            ($port, $path) = eval { open_port($env) };
            if ($port) {
                $select = IO::Select->new($port);
                ($buffer, $asked, $last_data) = ('', 0, time);
                $state->{port}   = @candidates > 1 ? "$path (1 of " . @candidates . ' ports, set AGENTDINO_SERIAL to choose)' : $path;
                $state->{status} = undef;
            } else {
                $state->{status} = '- waiting for the board';
                $redraw->();
                select(undef, undef, undef, 1);
                next;
            }
        }

        if (time - $asked >= LEASE_REFRESH) {
            $asked = time;
            unless (defined syswrite($port, "?\n") || $!{EAGAIN} || $!{EINTR}) {
                ($port, $select, $state->{status}) = (undef, undef, '- board disconnected');
                next;
            }
        }
        if ($select->can_read(0.5)) {
            my $read = sysread($port, my $chunk, 4096);
            unless (defined $read ? $read : $!{EAGAIN} || $!{EINTR}) {    # EOF or a dead port
                ($port, $select, $state->{status}) = (undef, undef, '- board disconnected');
                next;
            }
            if ($read) {
                $last_data = time;
                feed($state, \$buffer, $chunk, strftime('%H:%M:%S', localtime));
            }
        }
        my $quiet = time - $last_data;
        $state->{status} = $quiet >= STALE_AFTER ? "- no data for ${quiet}s" : undef;
        $redraw->();
    }
}

1;
