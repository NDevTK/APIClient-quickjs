function reached(){ fetch("/api/reached"); }
function neverCalled(){ var x=new XMLHttpRequest(); x.open("POST","/api/hidden"); x.send(); }
function alsoHidden(){ return fetch("/api/lazy?z="+location.search); }
reached();   // only this one actually runs
