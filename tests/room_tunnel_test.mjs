// Runs GhostRoomTests against a real LocalGhost room service on loopback.
// Usage: node room_tunnel_test.mjs <path to ghostRooms/service.mjs> <GhostRoomTests.exe>
import { spawn } from 'node:child_process';
import { pathToFileURL } from 'node:url';

const [servicePath, testExecutable] = process.argv.slice(2);
const { createService } = await import(pathToFileURL(servicePath).href);

const service = createService({ httpPort: 0, udpPort: 0, backendUrl: 'http://localhost:8080' });
try {
    const { httpPort } = await service.listen();
    const child = spawn(testExecutable, [`http://127.0.0.1:${httpPort}`], { stdio: 'inherit' });
    const timeout = setTimeout(() => child.kill(), 30000);
    try {
        process.exitCode = await new Promise((resolve, reject) => {
            child.once('error', reject);
            child.once('exit', code => resolve(code ?? 1));
        });
    } finally {
        clearTimeout(timeout);
    }
} finally {
    await service.close();
}
