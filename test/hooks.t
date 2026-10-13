# Keeps hooks.example.json valid: every event the firmware protocol needs, each calling signal.pl with its command.
use strict;
use warnings;
use FindBin qw($Bin);
use JSON::PP qw(decode_json);
use Test::More;

open(my $file, '<', "$Bin/../hooks.example.json") or die "open: $!";
my $hooks = decode_json(do { local $/; <$file> })->{hooks};

my %expected = (
    UserPromptSubmit   => 'OW',
    PostToolUse        => 'W',
    PostToolUseFailure => 'EW',
    SubagentStart      => 'S',
    SubagentStop       => 's',
    Stop               => 'D',
    Notification       => 'I',
    SessionEnd         => 'O',
);

is_deeply([ sort keys %$hooks ], [ sort keys %expected ], 'the example wires exactly the expected events');
for my $event (sort keys %expected) {
    my ($group) = @{ $hooks->{$event} };
    my ($hook) = @{ $group->{hooks} };
    is($hook->{command}, "/path/to/agentdino/signal.pl $expected{$event}", "$event runs signal.pl $expected{$event}");
    is($hook->{timeout}, 2, "$event has the 2 s timeout");
}
is($hooks->{Notification}[0]{matcher}, 'permission_prompt', 'only permission prompts mean input needed');

done_testing;
