let pressCount = 0;
let solved = 0;

var lastUserInput = "";
var lastChanged = "";
var lastReported = "";
var lastUpdated = "";


const correctOrder = [3, 2, 4, 5, 1]; // Define the correct order
let userInput = [];
const fingerElements = [
    document.getElementById('finger1'),
    document.getElementById('finger2'),
    document.getElementById('finger3'),
    document.getElementById('finger4'),
    document.getElementById('finger5')
];

function initializePage() {
    const audio = document.getElementById('riser-initial');
    if (!audio) return;

    let completedPlays = 0;

    const onEnded = () => {
        completedPlays++;
        if (completedPlays < 3) {
            audio.currentTime = 0;
            audio.play().catch(error => console.warn("Audio playback error:", error));
        } else {
            audio.removeEventListener('ended', onEnded);
        }
    };

    audio.addEventListener('ended', onEnded);
    audio.currentTime = 0;
    audio.play().catch(error => console.warn("Audio playback error:", error));
}

const RESET_RISER_FLAG = 'playRiserOnLoad';

function shouldPlayRiserFromResetCommand() {
    const shouldPlay = sessionStorage.getItem(RESET_RISER_FLAG) === '1';
    if (shouldPlay) {
        sessionStorage.removeItem(RESET_RISER_FLAG);
    }
    return shouldPlay;
}

function fingerClicked(finger) {
    if (solved == 1)
        return;

    const fingerElement = fingerElements[finger - 1];
    const audio = document.getElementById('finger-sound');
    if (audio) {
        audio.currentTime = 0;
        audio.play().catch(error => console.warn("Audio playback error:", error));
    }

    if (!fingerElement.classList.contains('selected') && pressCount < 5) {
        pressCount++;
        userInput.push(finger); // Add the clicked finger to the user input
        fingerElement.textContent = pressCount;
        fingerElement.classList.add('selected');
    }

    if (pressCount === 5) {
        setTimeout(checkAccess, 500); // Check access after the last input
    }
}

function checkAccess() {
    SendPostToHA(); // Send user input to HA
    const result = document.getElementById('result');
    if (JSON.stringify(userInput) === JSON.stringify(correctOrder)) {
        const audiog= document.getElementById('access-granted-sound');
        if (audiog) {
            audiog.currentTime = 0;
            audiog.play().catch(error => console.warn("Audio playback error:", error));
        }
        result.textContent = "ACCESS GRANTED";
        result.style.color = "white";
        solved = 1;
        // Trigger fade out and show skull
        const foreground = document.getElementById('foreground');
        const skull = document.getElementById('skull-image');

        // Step 1: Fade out foreground and fade in skull
        foreground.classList.add('fade-out');
        skull.style.opacity = 1;

        // Step 2: After 1 second, fade skull out and restore foreground
        setTimeout(() => {
            skull.style.opacity = 0;
            foreground.classList.remove('fade-out');
        }, 2000);        
        // no reset if granted
    } else {
        const audiod= document.getElementById('access-denied-sound');
        if (audiod) {
            audiod.currentTime = 0;
            audiod.play().catch(error => console.warn("Audio playback error:", error));
        }
        result.textContent = "ACCESS DENIED";
        result.style.color = "red";
        setTimeout(resetFingers, 2000); // Reset after showing result
    }
}

function resetFingers() {
    pressCount = 0;
    userInput = [];
    fingerElements.forEach(finger => {
        finger.textContent = '';
        finger.classList.remove('selected');
    });
    const result = document.getElementById('result');
    result.textContent = '';

    // Debajo quitar y poner el hover para 
    // que no quede un dedo como "agarrado"

    // Dynamically update the :hover color
    const styleSheet = document.styleSheets[0]; // Select the first stylesheet
    let hoverRule = null;

    // Find the existing hover rule and remove it
    for (let i = 0; i < styleSheet.cssRules.length; i++) {
        if (styleSheet.cssRules[i].selectorText === '.clickable:hover') {
            hoverRule = styleSheet.cssRules[i];
            styleSheet.deleteRule(i);
            break;
        }
    }
    // Add a new hover rule with a different color
    const newHoverColor = 'rgba(140, 0, 255.  0.11)'; // Example: red hover color
    styleSheet.insertRule(`.clickable:hover { background-color: ${newHoverColor}; }`, styleSheet.cssRules.length);

    // no es necesario hacer el reload
    // location.reload();    

}

$(document).ready(function () {
    if (shouldPlayRiserFromResetCommand()) {
        initializePage();
    }
});


// Send POST request to Home Assistant
function SendPostToHA() {
    var newUserInput = JSON.stringify(userInput);
    var newChanged = new Date().toISOString();
    var newReported = new Date().toISOString();
    var newUpdated = new Date().toISOString();
    const data = {
        event_type: "state_changed",
        entity_id: "custom_sensor.handscanner",
        event: {
            entity_id: "custom_sensor.handscanner",
            old_state: {
                entity_id: "custom_sensor.handscanner",
                state: lastUserInput,
                last_changed: lastChanged,
                last_reported: lastReported,
                last_updated: lastUpdated
            },
            new_state: {
                entity_id: "custom_sensor.handscanner",
                state: newUserInput,
                last_changed: newChanged,
                last_reported: newReported,
                last_updated: newUpdated
            }
        }
    };
    lastUserInput = newUserInput;
    lastChanged = newChanged;
    lastUpdated = newUpdated;
    lastReported = newReported;

    $.ajax({
        url: CONFIG.HA_EVENT_URL,
        type: "POST",
        headers: {
            "Content-Type": "application/json",
            "Authorization":  `Bearer ${CONFIG.BEARER_TOKEN}` // Replace with your token
        },
        data: JSON.stringify(data),
        success: function () {
            console.log("POST request sent successfully");
        },
        error: function (xhr, status, error) {
            console.error("Error sending POST request:", error);
        }
    });
}

// Function to check the endpoint and reset if needed
function checkResetEndpoint() {
    $.ajax({
        url: CONFIG.RESET_ENDPOINT_URL,
        type: "GET",
        success: function (response) {
            const command = String(response).trim();

            if (command === "1") {
                console.log("Reset command received. Restarting handscanner...");
                document.getElementById("belial-hint").style.opacity = 0;
                document.getElementById("belial-hint").style.pointerEvents = "none";                                
                document.getElementById("skull-image").style.opacity = 0;
                document.getElementById("foreground").style.visibility = "visible";
                document.getElementById("foreground").classList.remove("fade-out");                
                document.getElementById("blackout").style.display = "none";
                sessionStorage.setItem(RESET_RISER_FLAG, '1');
                window.location.replace(window.location.href);
                // location.reload(true);
            }
            else if (command === "2") {
                document.getElementById("belial-hint").style.opacity = 0;                
                document.getElementById("belial-hint").style.pointerEvents = "none";                                
                document.getElementById("skull-image").style.opacity = 0;
                document.getElementById("foreground").style.visibility = "visible";
                document.getElementById("foreground").classList.remove("fade-out");                
                console.log("Received command 2: blackout");
                document.getElementById("blackout").style.display = "block";
            }
            else if (command === "3") {
                console.log("Received command 3: show Belial hint");

                document.getElementById("foreground").style.visibility = "hidden";

                document.getElementById("blackout").style.display = "none";

                const belialHint = document.getElementById("belial-hint");
                if (belialHint) {
                    belialHint.style.opacity = 1;   // Fade in full screen
                }
            }            
            else
            {
                console.log("Recibido de api");
                console.log(command);
            }
        },
        error: function (xhr, status, error) {
            console.error("Error checking reset endpoint:", error);
        }
    });
}

// Start periodic check for the reset endpoint
setInterval(checkResetEndpoint, CONFIG.SECONDS_CHECKFORRESET); // Check every 10 seconds

