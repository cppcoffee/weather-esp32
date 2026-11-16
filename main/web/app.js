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

var voiceMode = document.getElementById('voice-mode');
var voiceCustom = document.getElementById('voice-custom');
var voiceUrl = document.getElementById('voice-url');
function updateVoiceMode() {
    var custom = voiceMode.value === 'custom';
    voiceCustom.disabled = !custom;
    voiceCustom.hidden = !custom;
    voiceUrl.required = custom;
}
voiceMode.onchange = updateVoiceMode;
updateVoiceMode();
