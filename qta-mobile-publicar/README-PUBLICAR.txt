QTA Mobile - publicar para abrir em qualquer celular

O arquivo principal fica em:
public/index.html

OPCAO 1 - Firebase Hosting

1. Abra o Prompt de Comando ou PowerShell nesta pasta:
   qta-mobile-publicar

2. Instale ou use o Firebase CLI:
   npm install -g firebase-tools

3. Entre na sua conta:
   firebase login

4. Ligue esta pasta ao seu projeto Firebase:
   firebase use --add

   Escolha o projeto do seu Realtime Database:
   qta-monitor

5. Publique:
   firebase deploy --only hosting

6. O Firebase vai mostrar um link parecido com:
   https://NOME-DO-PROJETO.web.app

Esse link abre no celular de qualquer lugar com internet.


OPCAO 2 - Netlify

1. Acesse:
   https://app.netlify.com/drop

2. Arraste a pasta:
   qta-mobile-publicar/public

3. O Netlify gera um link publico.


IMPORTANTE

Se a tela abrir mas nao ler dados, verifique as regras do Firebase Realtime Database.
Para teste publico, a regra de leitura precisa permitir leitura:

{
  "rules": {
    ".read": true,
    ".write": true
  }
}

Para uso real, nao deixe ".write": true aberto publicamente.


