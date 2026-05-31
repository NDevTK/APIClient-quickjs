window.addEventListener("message", function (e) {
  var a = [e.data.x];
  if (a.indexOf("y") >= 0) {
    document.getElementById("root").innerHTML = e.data.html;
  }
});
