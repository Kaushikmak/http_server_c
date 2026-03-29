document.addEventListener("DOMContentLoaded", () => {
    // --- SCROLL INTERSECTION OBSERVER ---
    const animatedElements = document.querySelectorAll('.fade-in, .slide-up');
    const observer = new IntersectionObserver((entries, obs) => {
        entries.forEach(entry => {
            if (entry.isIntersecting) {
                entry.target.classList.add('visible');
                obs.unobserve(entry.target);
            }
        });
    }, { threshold: 0.15 });
    animatedElements.forEach(el => observer.observe(el));

    // --- BACKGROUND ANIMATION (TOPOLOGICAL MESH) ---
    const bgCanvas = document.getElementById('bgCanvas');
    const bgCtx = bgCanvas.getContext('2d');
    let bgWidth, bgHeight;
    const particles = [];

    function resizeBg() {
        bgWidth = bgCanvas.width = window.innerWidth;
        bgHeight = bgCanvas.height = window.innerHeight;
    }
    window.addEventListener('resize', resizeBg);
    resizeBg();

    for (let i = 0; i < 70; i++) {
        particles.push({
            x: Math.random() * bgWidth,
            y: Math.random() * bgHeight,
            vx: (Math.random() - 0.5) * 0.4,
            vy: (Math.random() - 0.5) * 0.4,
            radius: Math.random() * 2 + 1
        });
    }

    function drawBackground() {
        bgCtx.clearRect(0, 0, bgWidth, bgHeight);
        bgCtx.fillStyle = '#58a6ff';
        bgCtx.strokeStyle = '#58a6ff';

        particles.forEach(p => {
            p.x += p.vx;
            p.y += p.vy;

            if (p.x < 0) p.x = bgWidth;
            if (p.x > bgWidth) p.x = 0;
            if (p.y < 0) p.y = bgHeight;
            if (p.y > bgHeight) p.y = 0;

            bgCtx.beginPath();
            bgCtx.arc(p.x, p.y, p.radius, 0, Math.PI * 2);
            bgCtx.fill();
        });

        for (let i = 0; i < particles.length; i++) {
            for (let j = i + 1; j < particles.length; j++) {
                const dx = particles[i].x - particles[j].x;
                const dy = particles[i].y - particles[j].y;
                const dist = Math.sqrt(dx * dx + dy * dy);

                if (dist < 150) {
                    bgCtx.globalAlpha = 1 - (dist / 150);
                    bgCtx.beginPath();
                    bgCtx.moveTo(particles[i].x, particles[i].y);
                    bgCtx.lineTo(particles[j].x, particles[j].y);
                    bgCtx.stroke();
                }
            }
        }
        bgCtx.globalAlpha = 1;
        requestAnimationFrame(drawBackground);
    }
    drawBackground();

    // --- FOREGROUND MINI-GAME (ROUTE PACKETS TO PROXY) ---
    const gameCanvas = document.getElementById('gameCanvas');
    const gCtx = gameCanvas.getContext('2d');
    const scoreElement = document.getElementById('scoreValue');
    
    let score = 0;
    let frameCount = 0;
    let packets = [];
    
    // Central Proxy Node
    const proxyNode = {
        x: gameCanvas.width / 2,
        y: gameCanvas.height / 2,
        radius: 25,
        color: '#58a6ff'
    };

    // User's Mouse (Routing Ring)
    const mouse = { x: -100, y: -100, radius: 40 };

    gameCanvas.addEventListener('mousemove', (e) => {
        const rect = gameCanvas.getBoundingClientRect();
        mouse.x = e.clientX - rect.left;
        mouse.y = e.clientY - rect.top;
    });

    gameCanvas.addEventListener('mouseleave', () => {
        mouse.x = -100;
        mouse.y = -100;
    });

    class GamePacket {
        constructor() {
            // Spawn randomly on edges
            if (Math.random() > 0.5) {
                this.x = Math.random() < 0.5 ? 0 : gameCanvas.width;
                this.y = Math.random() * gameCanvas.height;
            } else {
                this.x = Math.random() * gameCanvas.width;
                this.y = Math.random() < 0.5 ? 0 : gameCanvas.height;
            }
            this.vx = (Math.random() - 0.5) * 2;
            this.vy = (Math.random() - 0.5) * 2;
            this.radius = 5;
            this.color = '#c9d1d9'; // Default white/grey
            this.routed = false;
        }

        update() {
            this.x += this.vx;
            this.y += this.vy;

            // 1. Mouse Interaction (Routing to Proxy)
            if (!this.routed) {
                const dxM = mouse.x - this.x;
                const dyM = mouse.y - this.y;
                if (Math.sqrt(dxM*dxM + dyM*dyM) < mouse.radius) {
                    this.routed = true;
                    this.color = '#f0883e'; // Orange when routed
                    
                    // Aim directly at proxy
                    const dxP = proxyNode.x - this.x;
                    const dyP = proxyNode.y - this.y;
                    const distP = Math.sqrt(dxP*dxP + dyP*dyP);
                    this.vx = (dxP / distP) * 3; // Speed up towards proxy
                    this.vy = (dyP / distP) * 3;
                }
            }
        }

        draw() {
            gCtx.beginPath();
            gCtx.arc(this.x, this.y, this.radius, 0, Math.PI * 2);
            gCtx.fillStyle = this.color;
            gCtx.fill();
        }
    }

    function drawGame() {
        gCtx.fillStyle = '#000000';
        gCtx.fillRect(0, 0, gameCanvas.width, gameCanvas.height);

        // Draw Mouse Router Ring
        if (mouse.x > 0) {
            gCtx.beginPath();
            gCtx.arc(mouse.x, mouse.y, mouse.radius, 0, Math.PI * 2);
            gCtx.strokeStyle = 'rgba(88, 166, 255, 0.3)';
            gCtx.lineWidth = 2;
            gCtx.stroke();
        }

        // Draw Proxy Server
        gCtx.beginPath();
        gCtx.arc(proxyNode.x, proxyNode.y, proxyNode.radius, 0, Math.PI * 2);
        gCtx.fillStyle = proxyNode.color;
        gCtx.fill();
        gCtx.shadowBlur = 15;
        gCtx.shadowColor = proxyNode.color;
        gCtx.fill();
        gCtx.shadowBlur = 0; // Reset
        
        gCtx.fillStyle = '#161b22';
        gCtx.font = 'bold 12px sans-serif';
        gCtx.textAlign = 'center';
        gCtx.textBaseline = 'middle';
        gCtx.fillText('PROXY', proxyNode.x, proxyNode.y);

        // Spawn packets
        if (frameCount % 30 === 0) {
            packets.push(new GamePacket());
        }

        // Process Packets
        for (let i = packets.length - 1; i >= 0; i--) {
            let p = packets[i];
            p.update();
            p.draw();

            // Collision with Proxy Server
            const dx = proxyNode.x - p.x;
            const dy = proxyNode.y - p.y;
            if (Math.sqrt(dx*dx + dy*dy) < proxyNode.radius + p.radius) {
                // Packet absorbed by proxy!
                score++;
                scoreElement.innerText = score;
                
                // Proxy flashes green on cache hit
                proxyNode.color = '#2ea043';
                setTimeout(() => proxyNode.color = '#58a6ff', 150);
                
                packets.splice(i, 1);
                continue;
            }

            // Remove out of bounds packets
            if (p.x < -20 || p.x > gameCanvas.width + 20 || p.y < -20 || p.y > gameCanvas.height + 20) {
                packets.splice(i, 1);
            }
        }

        frameCount++;
        requestAnimationFrame(drawGame);
    }
    drawGame();
});