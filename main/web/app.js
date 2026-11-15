document.getElementById('rescan').onclick = function () {
    this.classList.add('busy');
};

var password = document.getElementById('pw');
var eye = document.getElementById('eye');
eye.onclick = function () {
    var reveal = password.type === 'password';
    password.type = reveal ? 'text' : 'password';
    eye.classList.toggle('revealed', reveal);
};
