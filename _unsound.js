var h = location.hash.slice(1).replace(/</g, "");
document.body.innerHTML = h;
