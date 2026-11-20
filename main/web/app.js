document.getElementById('rescan').onclick = function () {
    this.textContent = '扫描中…';
};

var password = document.getElementById('pw');
var eye = document.getElementById('eye');
eye.hidden = false;
eye.onclick = function () {
    var reveal = password.type === 'password';
    password.type = reveal ? 'text' : 'password';
    eye.classList.toggle('revealed', reveal);
    eye.setAttribute('aria-label', reveal ? '隐藏密码' : '显示密码');
};
