#!/usr/bin/gawk -nf

{
    isrepeat = 0;
}

$2 == "[HOT" && $3 != "identified" {
    if($4 + 0 == lastvaddr + 0x1000 && $9 == lastpaddr + 0x1000 && $11 == newpaddr + 0x1000) {
	noprint = 1;
	lastline = $0;
	isrepeat = 1;
    } else if (noprint) {
	print "...";
	print lastline;
	noprint = 0;
    }
    
    lastvaddr = $4;
    lastpaddr = $9;
    newpaddr = $11;
}

$2 == "[COLD" && $3 != "identified" {
    if($4 + 0 == lastcvaddr + 0x1000 && $9 == lastcpaddr + 0x1000 && $11 == newcpaddr + 0x1000) {
	noprint = 1;
	lastline = $0;
	isrepeat = 1;
    } else if (noprint) {
	print "...";
	print lastline;
	noprint = 0;
    }
    
    lastcvaddr = $4;
    lastcpaddr = $9;
    newcpaddr = $11;
}

$2 == "[THAW" {
    if($4 + 0 == lastthaw + 0x1000) {
	isrepeat = 1;
	noprint = 1;
	lastline = $0;
    } else if(noprint) {
	print "...";
	print lastline;
	noprint = 0;
    }
    
    lastthaw = $4;
}

{
    if(noprint && !isrepeat) {
	print "...";
	print lastline;
	noprint = 0;
    }
    
    if(!noprint) {
	print;
    }
}
