window.addEventListener("message", function (e) {
  var d = e.data;
  if (Object.getOwnPropertyNames(d).indexOf("html") >= 0) {
    document.getElementById("root").innerHTML = d.html;
  }
});
