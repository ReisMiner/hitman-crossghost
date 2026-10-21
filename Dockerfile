# syntax=docker/dockerfile:1

FROM node:22-bookworm-slim AS dependencies
WORKDIR /app
COPY Server/package.json Server/package-lock.json ./
RUN npm ci --omit=dev --ignore-scripts --no-audit --no-fund && npm cache clean --force

FROM node:22-bookworm-slim
WORKDIR /app
ENV NODE_ENV=production PORT=47047 BIND_HOST=0.0.0.0 UDP_PORT=30002 \
    PUBLIC_ORIGIN=https://hitmanghost.ramon.cc
COPY --from=dependencies --chown=node:node /app/node_modules ./node_modules
COPY --chown=node:node Server/ ./
# A fresh named volume inherits these templates and writable identity folders.
RUN node -e "const fs=require('fs');for(const game of ['h1','h2','h3'])for(const dir of ['users','steamids','epicids','gogids','xboxids'])fs.mkdirSync('userdata/'+game+'/'+dir,{recursive:true})" \
    && chown -R node:node /app/userdata
USER node
EXPOSE 47047/tcp 30002/udp
HEALTHCHECK --interval=15s --timeout=5s --start-period=10s --retries=3 \
    CMD node -e "const r=require('http').get('http://127.0.0.1:'+(process.env.PORT||47047)+'/healthz',s=>{s.resume();process.exit(s.statusCode===200?0:1)});r.setTimeout(3000,()=>{r.destroy();process.exit(1)});r.on('error',()=>process.exit(1))"
CMD ["node", "serb.js"]
