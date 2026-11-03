// SPDX-License-Identifier: GPL-3.0-only
// Synthetic packets to a loopback fixture; no executable or PS5 connection.
const fs=require('node:fs'),path=require('node:path'),net=require('node:net'),assert=require('node:assert/strict');
const {spawn,spawnSync}=require('node:child_process');
const root=path.resolve(__dirname,'..'),build=path.join(root,'.cache/launch-client-tests',String(Date.now()));
fs.mkdirSync(build,{recursive:true});
const executable=path.join(build,process.platform==='win32'?'client.exe':'client');
const sources=['native/src/launch_client.cpp','native/tests/launch_client_main.cpp'].map(p=>path.join(root,p));
const include=path.join(root,'native/include');
function run(cmd,args,options={}){const r=spawnSync(cmd,args,{cwd:build,stdio:'inherit',windowsHide:true,...options});if(r.error)throw r.error;assert.equal(r.status,0,'fixture compiler failed');}
if(process.platform==='win32'){
 const vswhere=path.join(process.env['ProgramFiles(x86)'],'Microsoft Visual Studio/Installer/vswhere.exe');
 const vs=spawnSync(vswhere,['-latest','-products','*','-requires','Microsoft.VisualStudio.Component.VC.Tools.x86.x64','-property','installationPath'],{encoding:'utf8',windowsHide:true});
 assert.equal(vs.status,0);const sdl=path.join(root,'.cache/native-deps/SDL2-2.32.10');
 const q=x=>{assert.ok(!/["%\r\n]/.test(x));return `"${x}"`;};
 const command=`call ${q(path.join(vs.stdout.trim(),'Common7/Tools/VsDevCmd.bat'))} -no_logo -arch=x64 -host_arch=x64 >nul && cl.exe /nologo /std:c++17 /O2 /EHsc /utf-8 /D_CRT_SECURE_NO_WARNINGS /W4 /WX /I${q(include)} /I${q(path.join(sdl,'include'))} ${sources.map(q).join(' ')} /Fe:client.exe /link /LIBPATH:${q(path.join(sdl,'lib/x64'))} SDL2.lib ws2_32.lib`;
 run(process.env.ComSpec,['/d','/s','/c',`"${command}"`],{windowsVerbatimArguments:true});
 fs.copyFileSync(path.join(sdl,'lib/x64/SDL2.dll'),path.join(build,'SDL2.dll'));
}else{
 const flags=spawnSync('sdl2-config',['--cflags','--libs'],{encoding:'utf8'});assert.equal(flags.status,0);
 run('c++',['-std=c++17','-O2','-Wall','-Wextra','-Werror','-I',include,...sources,...flags.stdout.trim().split(/\s+/),'-o',executable]);
}
async function scenario(mode){
 const directory=path.join(build,mode);fs.mkdirSync(directory);
 const request=path.join(directory,'launch-request.txt'),status=path.join(directory,'launch-status.txt');
 let connections=0,received=0,timer;const sockets=new Set();
 const server=net.createServer(socket=>{
  connections++;sockets.add(socket);socket.on('error',()=>{});socket.on('close',()=>sockets.delete(socket));
  socket.on('data',bytes=>{received+=bytes.length;});
  socket.on('end',()=>{
   if(mode==='probe')return;
   if(mode==='timeout')return;
   const text=fs.readFileSync(request,'utf8'),id=/^id=(\w+)$/m.exec(text)[1],issued=/^issued=(\d+)$/m.exec(text)[1];
   fs.unlinkSync(request);
   const answer=()=>fs.writeFileSync(status,`console-shelf-status 1\nid=${mode==='wrong-id'?'f'.repeat(32):id}\nissued=${issued}\nstate=ready\ncode=00000000\n`);
   if(mode==='late-ready-cancel')timer=setTimeout(answer,500);else answer();
  });
 });
 await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));const port=server.address().port;
 if(mode==='busy'||mode==='stale'){
  fs.writeFileSync(request,'earlier owned request');
  if(mode==='stale'){const earlier=new Date(Date.now()-60000);fs.utimesSync(request,earlier,earlier);}
 }
 if(mode==='terminal-unconfirmed'){
  const issued=Math.floor(Date.now()/1000);
  fs.writeFileSync(status,`console-shelf-status 1\nid=${'a'.repeat(32)}\nissued=${issued}\nstate=unconfirmed\ncode=00000018\n`);
 }
 if(mode==='closed')await new Promise(resolve=>server.close(resolve));
 try{
  const result=await new Promise((resolve,reject)=>{
   const child=spawn(executable,[directory,String(port),mode],{cwd:root,windowsHide:true});let stdout='',stderr='';
   const deadline=setTimeout(()=>{child.kill();reject(new Error('Client fixture timeout: '+mode));},9000);
   child.stdout.on('data',b=>stdout+=b);child.stderr.on('data',b=>stderr+=b);child.on('error',reject);
   child.on('close',code=>{clearTimeout(deadline);resolve({code,stdout,stderr});});
  });
  assert.equal(result.code,0,mode+': '+result.stderr+result.stdout);
  assert.equal(JSON.parse(result.stdout.trim()).helperExecuted,false);
  const text=fs.existsSync(status)?fs.readFileSync(status,'utf8'):'';
  if(['success','stale','terminal-unconfirmed'].includes(mode))assert.match(text,/state=committed/);else assert.doesNotMatch(text,/state=committed/);
  if(mode==='probe'){
   assert.equal(connections,1,'probe sends one loopback transfer');
   assert.equal(received,64,'probe sends exactly the supplied ELF bytes');
   assert.ok(!fs.existsSync(request),'probe does not create a game launch request');
   assert.ok(!fs.existsSync(status),'probe does not create a game launch status');
  }
  if(mode==='busy')assert.equal(fs.readFileSync(request,'utf8'),'earlier owned request');else assert.ok(!fs.existsSync(request));
  if(mode==='stale')assert.equal(fs.readFileSync(path.join(directory,'launch-request.previous'),'utf8'),'earlier owned request');
  if(mode==='wrong-id')assert.match(text,new RegExp('id='+'f'.repeat(32)),'foreign reply must remain untouched');
  if(['closed','invalid','busy'].includes(mode))assert.equal(connections,0);else {assert.equal(connections,1);assert.equal(received,64);}
  return {mode,connections,received,passed:true};
 }finally{
  if(timer)clearTimeout(timer);for(const socket of sockets)socket.destroy();
  if(server.listening)await new Promise(resolve=>server.close(resolve));
 }
}
(async()=>{
 const results=[];
 for(const mode of ['success','cancel-ready','late-ready-cancel','closed','timeout','wrong-id','busy','stale','terminal-unconfirmed','invalid','probe'])results.push(await scenario(mode));
 fs.writeFileSync(path.join(root,'.cache/launch-client-tests/latest.json'),JSON.stringify({directory:build,helperExecuted:false,results},null,2)+'\n');
 console.log('PASS: eleven loopback transport fixtures: game handoff, cancellation, stale recovery, terminal-unconfirmed reuse and pure one-send probe without game request. No console/helper code executed.');
})().catch(error=>{console.error(error);process.exitCode=1;});
