#!/usr/bin/perl
# SPDX-License-Identifier: GPL-2.0-or-later
#
# wl.pl - write workloads and the hash verifier of the disconnect suite (T-M5-04, S-DISC-*).
# Runs in the macOS guest (system perl, core modules only).
#
#   wl.pl base   MP MARKDIR SEED    write the baseline files, fsync every file, its directory and MP, then
#                                   record "relpath sha256" lines in MARKDIR/base.list (after the fsyncs)
#   wl.pl seq    MP MARKDIR SEED    large sequential writes (64 KiB chunks), no fsync, until killed
#   wl.pl small  MP MARKDIR SEED    many small file creates in many directories, until killed
#   wl.pl churn  MP MARKDIR SEED    create / rename / mkdir / truncate / unlink churn, until killed
#   wl.pl fsync  MP MARKDIR SEED [HOLD]  write a file, fsync it and its directory, record MARKDIR/fs.K, repeat;
#                                   with HOLD=K the writer then sits idle (10 ms polls) until MARKDIR/go exists,
#                                   so a yank lands right after the fsyncs of file K returned
#   wl.pl verify MP MARKDIR         hash every file recorded in MARKDIR/base.list and MARKDIR/fs.*;
#                                   prints OK n, or BAD lines; exit 1 on a mismatch, a missing file or
#                                   an empty list
# The workload modes write their progress (operations done) to MARKDIR/progress.
use strict;
use warnings;
use Digest::SHA qw(sha256_hex sha256);
use IO::Handle;

my ($mode, $mp, $md, $seed, $hold) = @ARGV;
die "usage: wl.pl MODE MP MARKDIR [SEED]\n" unless defined $md;
$seed = 1 unless defined $seed;
my $ops = 0;

# deterministic, fast pseudo-random data: SHA-256 blocks of (seed, label, counter)
sub data {
	my ($label, $n) = @_;
	my $out = '';
	my $i = 0;
	$out .= sha256("$seed/$label/" . $i++) while length($out) < $n;
	return substr($out, 0, $n);
}
sub progress {
	$ops++;
	return if $ops != 1 && $ops % 8;
	if (open(my $p, '>', "$md/progress.tmp")) { print $p "$ops\n"; close($p); rename("$md/progress.tmp", "$md/progress"); }
}
sub fsync_path {
	my ($path) = @_;
	open(my $h, '<', $path) or die "open $path: $!";
	$h->sync or die "fsync $path: $!";
	close($h);
}
sub write_synced { # write FILE, fsync it and its directory; returns the sha
	my ($file, $dir, $bytes) = @_;
	open(my $h, '>', $file) or die "create $file: $!";
	binmode($h);
	print $h $bytes or die "write $file: $!";
	$h->flush;
	$h->sync or die "fsync $file: $!";
	close($h) or die "close $file: $!";
	fsync_path($dir);
	return sha256_hex($bytes);
}

if ($mode eq 'base') {
	mkdir("$mp/base") or die "mkdir base: $!";
	my @sizes = (100, 5000, 70000, 1048576, 333333);
	my @rec;
	my $k = 0;
	# the sentinel carries a magic string, so that a control can find its place in the image
	my $sent = "DISCSENTINEL01" . data("sentinel", 4096);
	push @rec, ["base/sentinel.bin", write_synced("$mp/base/sentinel.bin", "$mp/base", $sent)];
	for my $s (@sizes) {
		my $name = "base/f" . $k++ . ".bin";
		push @rec, [$name, write_synced("$mp/$name", "$mp/base", data($name, $s))];
	}
	fsync_path($mp);
	open(my $l, '>', "$md/base.list.tmp") or die;
	print $l "$_->[0] $_->[1]\n" for @rec;
	close($l);
	rename("$md/base.list.tmp", "$md/base.list") or die;
	exit 0;
}
if ($mode eq 'seq') {
	my $n = 0;
	while (1) {
		open(my $h, ">", "$mp/big.bin") or die "create: $!"; # one name: the volume never fills
		binmode($h);
		my $chunk = data("seq$n", 65536);
		for (1 .. 400) { print $h $chunk or die "write: $!"; progress(); }
		close($h);
		$n++;
	}
}
if ($mode eq 'small') {
	my $d = 0;
	while (1) {
		mkdir("$mp/s$d") or die "mkdir: $!";
		for my $i (1 .. 200) {
			open(my $h, '>', "$mp/s$d/f$i") or die "create: $!";
			print $h data("s$d/$i", 200 + (($i * 97) % 2800));
			close($h);
			progress();
		}
		$d++;
	}
}
if ($mode eq 'churn') {
	mkdir("$mp/churn") or die "mkdir: $!";
	my $i = 0;
	while (1) {
		my $f = "$mp/churn/a$i";
		open(my $h, '>', $f) or die "create: $!";
		print $h data("c$i", 1000 + ($i % 7) * 3000);
		close($h);
		rename($f, "$mp/churn/b$i") or die "rename: $!";
		if ($i % 5 == 0) {
			mkdir("$mp/churn/d$i") or die "mkdir: $!";
			rename("$mp/churn/b$i", "$mp/churn/d$i/c") or die "rename into dir: $!";
			rename("$mp/churn/d$i", "$mp/churn/e$i") or die "rename dir: $!";
		}
		if ($i % 3 == 0 && $i % 5 != 0) { truncate("$mp/churn/b$i", 10); chmod(0600, "$mp/churn/b$i"); utime(1000000000, 1000000000, "$mp/churn/b$i"); }
		if ($i >= 12) { my $o = $i - 12; unlink("$mp/churn/b$o"); unlink("$mp/churn/e$o/c"); rmdir("$mp/churn/e$o"); }
		$i++;
		progress();
	}
}
if ($mode eq 'fsync') {
	mkdir("$mp/fsync") or die "mkdir: $!";
	fsync_path($mp);
	my $k = 1;
	while (1) {
		my $name = "fsync/g$k.bin";
		my $sha = write_synced("$mp/$name", "$mp/fsync", data($name, 131072 + $k * 4096));
		# the record is made after both fsyncs returned: a record is a durability claim
		open(my $l, '>', "$md/fs.$k.tmp") or die;
		print $l "$name $sha\n";
		close($l);
		rename("$md/fs.$k.tmp", "$md/fs.$k") or die;
		if (defined $hold && $k == $hold) { select(undef, undef, undef, 0.01) until -e "$md/go"; }
		$k++;
		progress();
		# unsynced tail traffic between the fsyncs
		open(my $h, '>', "$mp/fsync/tail$k") or die;
		print $h data("tail$k", 200000);
		close($h);
	}
}
if ($mode eq 'verify') {
	my @recs;
	for my $f (glob("$md/base.list"), sort glob("$md/fs.[0-9]*")) {
		next if $f =~ /\.tmp$/;
		open(my $l, '<', $f) or next;
		while (<$l>) { chomp; push @recs, [split / /] if length; }
		close($l);
	}
	my $bad = 0;
	for my $r (@recs) {
		my ($name, $sha) = @$r;
		my $got;
		if (open(my $h, '<', "$mp/$name")) {
			binmode($h);
			local $/;
			my $c = <$h>;
			$got = sha256_hex(defined $c ? $c : '');
			close($h);
		}
		if (!defined $got) { print "BAD $name: missing\n"; $bad++; }
		elsif ($got ne $sha) { print "BAD $name: sha256 $got, expected $sha\n"; $bad++; }
	}
	if (!@recs) { print "BAD: no record to verify\n"; exit 1; }
	print "OK " . scalar(@recs) . "\n" unless $bad;
	exit($bad ? 1 : 0);
}
die "unknown mode $mode\n";
