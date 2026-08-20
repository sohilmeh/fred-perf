#!/usr/bin/perl

use strict;
use Statistics::Descriptive;

sub do_analyze($$@) {
    my($title,$filter,@data) = @_;
    my $stat = Statistics::Descriptive::Full->new();
    $stat->add_data(@data);
    $stat->presorted(1);
    printf '"%s","%s",%d,%.3f,%.3f', $filter, $title,
	$stat->count(),	$stat->mean(), $stat->standard_deviation();
    # Quartiles (quartile cutoffs)
    for (my $q = 0; $q <= 4; $q++) {
	printf ',%.3f', $stat->quantile($q);
    }
    print "\n";
}

sub analyze($@) {
    my($title,@data) = @_;

    @data = sort { $a <=> $b } @data;

    do_analyze($title, 'raw', @data);

    my $trim = scalar(@data) >> 3;
    do_analyze($title, '6/8', @data[$trim..$#data-$trim]);

    my $trim = scalar(@data) >> 2;
    do_analyze($title, '2/4', @data[$trim..$#data-$trim]);
}

my @clks;
my @tcks;
my @nsec;

foreach my $file (@ARGV) {
    open(my $fh, '<', $file) or die "$0: $file: $!\n";

    my $tscspeed = 0.0;
    my $clkspeed = 0.0;
    my $ncalls = 1000;
    while (defined(my $l = <$fh>)) {
	if ($l =~ /^\"Core MHz:\",([0-9.]+)/) {
	    $clkspeed = $1 + 0.0;
	} elsif ($l =~ /^\"TSC MHz:\",([0-9.]+)/) {
	    $tscspeed = $1 + 0.0;
	} elsif ($l =~ /^\"Calls\/loop:\",([0-9]+)/) {
	    $ncalls = $1 + 0;
	} elsif ($l =~ /^([0-9]+),([0-9]+)/) {
	    # An actual data point; if it has a decimal point it is
	    # a statistic
	    my $nt = ($2 + 0.0)/$ncalls;
	    my $ns = $nt * 1000.0/$tscspeed;
	    push(@tcks, $nt);
	    push(@nsec, $ns);
	    my $ratio = $clkspeed/$tscspeed;
	    # If the ratio is preposterous, ignore it
	    if ($ratio >= 0.5 && $ratio <= 2.0) {
		my $nc = $nt * $ratio;
		push(@clks, $nc);
	    }
	}
    }

    close($fh);
}

print '"Filter","Unit","Samples","Mean","Sigma","Min","Q1","Median","Q3","Max"', "\n";

analyze('ticks', @tcks);
analyze('nsecs', @nsec);
analyze('clocks', @clks);
