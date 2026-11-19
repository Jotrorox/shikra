#!/usr/bin/perl
# limit.pl -- portable wall-clock limit for the benchmark harness (macOS has no
# timeout(1)).  Usage:
#
#   perl limit.pl SECONDS IN OUT ERR RUSAGE -- command [args...]
#
# Runs `/usr/bin/time -l -o RUSAGE command args...` in its own process group
# with stdin/stdout/stderr redirected from/to IN/OUT/ERR.  After SECONDS of
# wall-clock time the whole process group is killed (SIGKILL).  Prints one line
# "status=N wall=S" on stdout, where N is the command's exit status (124 on
# timeout, 128+signal if killed by a signal) and S the elapsed wall-clock
# seconds; exits with N.
use strict;
use warnings;
use POSIX ();
use Time::HiRes qw(time sleep);

my ($limit, $in, $out, $err, $rusage, $sep, @cmd) = @ARGV;
die "usage: limit.pl SECONDS IN OUT ERR RUSAGE -- cmd...\n"
    unless defined $sep && $sep eq '--' && @cmd;

my $t0 = time;
my $pid = fork();
die "fork: $!\n" unless defined $pid;
if ($pid == 0) {
    setpgrp(0, 0);
    open(STDIN, '<', $in) or die "stdin $in: $!\n";
    open(STDOUT, '>', $out) or die "stdout $out: $!\n";
    open(STDERR, '>', $err) or die "stderr $err: $!\n";
    exec('/usr/bin/time', '-l', '-o', $rusage, @cmd) or POSIX::_exit(127);
}

my $status;
my $timed_out = 0;
while (1) {
    my $r = waitpid($pid, POSIX::WNOHANG());
    if ($r == $pid) { $status = $?; last; }
    if (time - $t0 > $limit) {
        kill('KILL', -$pid);
        kill('KILL', $pid);
        waitpid($pid, 0);
        $timed_out = 1;
        last;
    }
    sleep(0.02);
}
my $wall = time - $t0;
my $code;
if ($timed_out) { $code = 124; }
elsif ($status & 127) { $code = 128 + ($status & 127); }
else { $code = $status >> 8; }
# /usr/bin/time exits with the child's status; if the child died of a signal,
# time reports it on its own stderr and exits non-zero -- keep its code.
printf "status=%d wall=%.3f\n", $code, $wall;
exit($code > 255 ? 255 : $code);
