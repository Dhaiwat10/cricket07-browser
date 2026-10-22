# Netplay room server

Online play needs a small server where players meet: it hands out tickets, relays the lobby, and
relays inputs when two players can't connect to each other directly. This folder is that server, as a
Cloudflare Worker with a Durable Object per room. It never sees the game: no page, no build, no files.

```bash
cd server
npx wrangler deploy
npx wrangler secret put NETPLAY_SECRET    # any random string; signs the room tickets
# optional: Cloudflare Realtime TURN, for players whose networks block direct connections
npx wrangler secret put TURN_KEY_ID
npx wrangler secret put TURN_KEY_API_TOKEN
```

Then open the game page with `?netplay=https://cricket07-netplay.<your-subdomain>.workers.dev`. Both
players use the same server URL, and both must run the same build from the same game files.
