// Custom API for a future native bridge; the stock game does not call these routes.
const express = require('express');
const jwt = require('jsonwebtoken');
const { GhostLobbyStore, LobbyError } = require('./ghostLobbyStore.js');

const store = new GhostLobbyStore();
const router = express.Router();
const woaAppIds = {
    steam: ['1659040', '1847520', '2183750'],
    epic: ['fghi4567xQOCheZIin0pazB47qGUvZw4'],
};

router.use((req, res, next) => {
    res.set('Cache-Control', 'no-store');
    // Resource URLs can bypass LocalGhost's outer authentication middleware.
    if (!req.jwt) return res.status(401).json({ error: { code: 'AUTH_REQUIRED', message: 'A bearer token is required.' } });
    // Legacy middleware only decodes JWTs. Verify server-issued tokens for this API,
    // without changing the stock game's legacy endpoints. The legacy secret and
    // storefront login remain development-only, not suitable for public hosting.
    try {
        const header = ['Authorization', 'X-GPS-Authorization'].map(name => req.get(name))
            .find(value => value && /^Bearer [^ ]+$/i.test(value));
        req.jwt = jwt.verify(header && header.split(' ')[1], 'secret', {
            algorithms: ['HS256'], issuer: 'auth.hitman.io', audience: 'pc_prod_7',
        });
    } catch (error) {
        return res.status(401).json({ error: { code: 'INVALID_TOKEN', message: 'A valid LocalGhost bearer token is required.' } });
    }
    if (req.gameVersion !== 'h3' || !Object.hasOwn(woaAppIds, req.jwt.platform) || !woaAppIds[req.jwt.platform].includes(req.jwt.pis)) {
        return res.status(403).json({ error: { code: 'WOA_REQUIRED', message: 'This API requires a WOA Steam or Epic profile.' } });
    }
    next();
});
router.use(express.json({ limit: '8kb', strict: true }));

const operations = {
    GetCapabilities: (actor, body) => store.getCapabilities(actor, body),
    Create: (actor, body) => store.create(actor, body),
    Join: (actor, body) => store.join(actor, body),
    GetLobby: (actor, body) => store.get(actor, body),
    Heartbeat: (actor, body) => store.get(actor, body, true),
    SetMap: (actor, body) => store.setMap(actor, body),
    SetReady: (actor, body) => store.setReady(actor, body),
    Start: (actor, body) => store.start(actor, body),
    Leave: (actor, body) => store.leave(actor, body),
};

for (const [name, operation] of Object.entries(operations)) {
    router.post(`/${name}`, (req, res, next) => {
        try {
            // Player identity is never accepted from the request body.
            const actor = { playerId: req.jwt.unique_name, platform: req.jwt.platform };
            res.json(operation(actor, req.body));
        } catch (error) {
            next(error);
        }
    });
}

router.use((error, req, res, next) => {
    if (error instanceof LobbyError) {
        return res.status(error.status).json({ error: { code: error.code, message: error.message } });
    }
    if (error.type === 'entity.parse.failed' || error.type === 'entity.too.large') {
        return res.status(error.type === 'entity.too.large' ? 413 : 400).json({
            error: { code: 'INVALID_JSON', message: 'Expected a JSON object no larger than 8 KiB.' },
        });
    }
    console.error('Unexpected GhostLobbyService error:', error.message);
    res.status(500).json({ error: { code: 'INTERNAL_ERROR', message: 'Lobby operation failed.' } });
});

module.exports = { router, cleanup: () => store.cleanup() };
