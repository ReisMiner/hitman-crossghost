import { test } from 'node:test';
import assert from 'node:assert/strict';
import { once } from 'node:events';
import { copyFile, mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import express from 'express';
import menuData from '../../components/h3/menuData.js';
import multiplayer from '../../components/h3/multiplayerHandler.js';

const root = fileURLToPath(new URL('../../', import.meta.url));
const hawkesBay = '8fcf8f5d-0fe9-47d7-bff3-d747d0cd1e89';
const haven = '5e895adf-9879-44d9-b43f-13bb24e897bc';
const paris = '59d1caab-2056-4f4b-a921-9958ef0a88eb';
const addedMaps = [
    { id: hawkesBay, title: 'UI_LOCATION_NEWZEALAND_CITY', location: 'LOCATION_NEWZEALAND',
        resources: 9, targetBrick: 'assembly:/_pro/scenes/missions/sheep/versus_targets_newzealand.brick' },
    { id: haven, title: 'UI_LOCATION_OPULENT_CITY', location: 'LOCATION_OPULENT_STINGRAY',
        resources: 6, targetBrick: 'assembly:/_pro/scenes/missions/opulent/mission_stingray/versus_targets_opulent_stingray.brick' },
    { id: paris, title: 'UI_LOCATION_PARIS_CITY', location: 'LOCATION_PARIS',
        resources: 6, targetBrick: 'assembly:/_pro/scenes/missions/paris/versus_targets_paris.brick' },
];
const presetPath = 'menudata/h3/menudata/multiplayerpresets.json';
const json = async relative => JSON.parse(await readFile(path.join(root, relative), 'utf8'));

test('H3 presets reference complete Ghost Mode contracts, including Hawke\'s Bay, Haven and Paris', async () => {
    const presets = await json(presetPath);
    assert.equal(new Set(presets.map(p => p.Id)).size, presets.length, 'unique preset IDs');
    for (const preset of presets) {
        for (const id of preset.Data.Contracts) {
            const contract = await json(`contractdata/${id}.json`);
            assert.equal(contract.Metadata.Id, id);
            assert.equal(contract.Metadata.IsVersus, true);
            assert.equal(contract.Metadata.Type, 'vsrace');
            assert.ok(contract.Metadata.Enginemodes.includes('multiplayer'));
            assert.equal(contract.Metadata.CharacterSetup[0].Characters.length, 2);
        }
    }
    for (const map of addedMaps) {
        const dedicated = presets.find(p => p.Id === map.id);
        assert.equal(dedicated.Metadata.Title, map.title);
        assert.equal(dedicated.Data.Properties.mode, 'invite');
        assert.deepEqual(dedicated.Data.Contracts, [map.id]);
        for (const preset of presets.filter(p => p.Metadata.IsDefault)) {
            assert.ok(preset.Data.Contracts.includes(map.id), `all-map pools include ${map.title}`);
        }
    }

    const contract = await json(`contractdata/${hawkesBay}.json`);
    assert.equal(contract.Metadata.Location, 'LOCATION_NEWZEALAND');
    assert.equal(contract.Metadata.ScenePath, 'assembly:/_pro/scenes/missions/sheep/scene_sheep.entity');
    assert.equal(contract.Metadata.EndConditions.PointLimit, 5);
    for (const brick of ['outfits_anubis', 'versus_targets_newzealand', 'versus_items_newzealand',
        'versus_entrance_newzealand', 'override_skipintro', 'lighting_day']) {
        assert.ok(contract.Data.Bricks.includes(`assembly:/_pro/scenes/missions/sheep/${brick}.brick`));
    }
    assert.ok(!contract.Data.VR, 'do not inherit Miami-only VR bricks');

    // Contract files are denied by default in both source control and Docker.
    // Without these exceptions a local test passes but the VPS loses the map.
    for (const ignoreFile of ['.gitignore', '.dockerignore']) {
        const rules = await readFile(path.join(root, '..', ignoreFile), 'utf8');
        for (const map of addedMaps) {
            assert.ok(rules.split(/\r?\n/).includes(`!Server/contractdata/${map.id}.json`),
                `${ignoreFile} must include the shipped ${map.title} contract`);
        }
    }
});

test('Haven uses the base Stingray scene and only the merged Ghost Mode bricks', async () => {
    const contract = await json(`contractdata/${haven}.json`);
    assert.equal(contract.Metadata.Location, 'LOCATION_OPULENT_STINGRAY');
    assert.deepEqual(contract.Metadata.Entitlements, ['LOCATION_OPULENT']);
    assert.equal(contract.Metadata.ScenePath,
        'assembly:/_pro/scenes/missions/opulent/mission_stingray/scene_stingray_basic.entity');
    assert.equal(contract.Metadata.EndConditions.PointLimit, 5);
    assert.deepEqual(contract.Data.Bricks, [
        'assembly:/_pro/scenes/missions/opulent/mission_stingray/versus_targets_opulent_stingray.brick',
        'assembly:/_pro/scenes/missions/opulent/mission_stingray/versus_items_opulent_stingray.brick',
        'assembly:/_pro/scenes/missions/opulent/mission_stingray/versus_entrance_opulent_stingray.brick',
        'assembly:/_PRO/Scenes/bricks/ui_versus.brick',
        'assembly:/_PRO/Scenes/bricks/vs_race_multiplayer.brick',
    ]);
    assert.ok(!contract.Data.VR, 'do not inherit another location\'s VR bricks');
    const original = await json(`contractdata/${hawkesBay}.json`);
    assert.deepEqual(contract.Data.Objectives[0].Definition, original.Data.Objectives[0].Definition);
});

test('Paris uses the native Showstopper scene and first-party Ghost Mode bricks', async () => {
    const contract = await json(`contractdata/${paris}.json`);
    assert.equal(contract.Metadata.Location, 'LOCATION_PARIS');
    assert.deepEqual(contract.Metadata.Entitlements, ['LOCATION_PARIS']);
    assert.equal(contract.Metadata.ScenePath, 'assembly:/_pro/scenes/missions/paris/_scene_paris.entity');
    assert.equal(contract.Metadata.EndConditions.PointLimit, 5);
    assert.deepEqual(contract.Data.Bricks, [
        'assembly:/_pro/scenes/missions/paris/versus_targets_paris.brick',
        'assembly:/_pro/scenes/missions/paris/versus_items_paris.brick',
        'assembly:/_pro/scenes/missions/paris/versus_entrance_paris.brick',
        'assembly:/_PRO/Scenes/bricks/ui_versus.brick',
        'assembly:/_pro/scenes/bricks/vs_race_multiplayer.brick',
    ]);
    const original = await json(`contractdata/${hawkesBay}.json`);
    assert.deepEqual(contract.Data.Objectives[0].Definition, original.Data.Objectives[0].Definition,
        'Paris uses the same versus target-picking objective');
});

test('both storefronts receive the added maps in menu/resources and join the same match', async t => {
    // Isolated profile and data copies: never read or modify a player's profile.
    const fixture = await mkdtemp(path.join(tmpdir(), 'ghost-maps-test-'));
    const previousDirectory = process.cwd();
    t.after(async () => {
        process.chdir(previousDirectory);
        await rm(fixture, { recursive: true, force: true });
    });
    const presets = await json(presetPath);
    const contracts = new Set(presets.flatMap(p => p.Data.Contracts));
    for (const relative of [presetPath, 'userdata/h3/allunlockables.json',
        ...[...contracts].map(id => `contractdata/${id}.json`)]) {
        const destination = path.join(fixture, relative);
        await mkdir(path.dirname(destination), { recursive: true });
        await copyFile(path.join(root, relative), destination);
    }
    const profileIds = {
        epic: '11111111-1111-4111-8111-111111111111',
        steam: '22222222-2222-4222-8222-222222222222',
    };
    await mkdir(path.join(fixture, 'userdata/h3/users'), { recursive: true });
    for (const profileId of Object.values(profileIds)) {
        await writeFile(path.join(fixture, `userdata/h3/users/${profileId}.json`),
            JSON.stringify(await json('userdata/h3/default.json')));
    }
    process.chdir(fixture);

    const app = express();
    app.use((req, res, next) => {
        req.gameVersion = 'h3';
        const store = req.header('X-Test-Store') || 'epic';
        req.jwt = { unique_name: profileIds[store], platform: store };
        next();
    });
    app.use(menuData.router);
    app.use(multiplayer);
    const server = app.listen(0, '127.0.0.1');
    t.after(() => new Promise(resolve => server.close(resolve)));
    await once(server, 'listening');
    const origin = `http://127.0.0.1:${server.address().port}`;
    const post = (endpoint, body, store = 'epic') => fetch(origin + endpoint, {
        method: 'POST', headers: { 'Content-Type': 'application/json', 'X-Test-Store': store },
        body: JSON.stringify(body), signal: AbortSignal.timeout(3000),
    });

    for (const store of ['epic', 'steam']) {
        const menu = await fetch(origin + '/multiplayerpresets?gamemode=versus', {
            headers: { 'X-Test-Store': store }, signal: AbortSignal.timeout(3000),
        });
        assert.equal(menu.status, 200);
        const { data } = await menu.json();
        for (const map of addedMaps) {
            assert.ok(data.Presets.some(p => p.Id === map.id));
            const userCentric = data.UserCentricContracts.find(c => c.Contract.Metadata.Id === map.id);
            assert.ok(userCentric, 'contract survives user-centric generation');
            assert.equal(userCentric.Data.LocationId, map.location);
            assert.equal(userCentric.Data.IsLocked, false);
            const response = await post('/GetRequiredResourcesForPreset', { id: map.id }, store);
            assert.equal(response.status, 200);
            const resources = await response.json();
            assert.equal(resources.length, 1);
            assert.equal(resources[0].Id, map.id);
            assert.equal(resources[0].Resources.length, map.resources);
            assert.ok(resources[0].Resources.includes(map.targetBrick));
        }
    }

    // No changes to transport or match registration are needed for the new map.
    for (const map of addedMaps) {
        for (const [hostStore, guestStore] of [['epic', 'steam'], ['steam', 'epic']]) {
            const host = await post('/RegisterToMatch', {
                presetId: map.id, matchId: '00000000-0000-0000-0000-000000000000',
            }, hostStore);
            assert.equal(host.status, 200);
            const { MatchId } = await host.json();
            const guest = await post('/RegisterToMatch', { presetId: map.id, matchId: MatchId }, guestStore);
            assert.equal(guest.status, 200);
            assert.equal((await guest.json()).MatchId, MatchId);
            const matches = await (await fetch(origin + '/DebugMatches')).json();
            const match = matches.find(m => m.matchId === MatchId);
            assert.equal(match.matchData.contractId, map.id);
            assert.deepEqual(match.players, [profileIds[hostStore], profileIds[guestStore]]);
        }
    }
});
